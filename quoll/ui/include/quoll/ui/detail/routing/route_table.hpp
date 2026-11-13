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

#ifndef QUOLL_UI_DETAIL_ROUTING_ROUTE_TABLE_HPP
#define QUOLL_UI_DETAIL_ROUTING_ROUTE_TABLE_HPP

#include <quoll/http/detail/percent.hpp>
#include <quoll/http/detail/route_template.hpp>
#include <quoll/http/fixed_string.hpp>
#include <quoll/ui/component/component.hpp>
#include <quoll/ui/detail/routing/route_field.hpp>
#include <quoll/ui/view/element.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::ui::detail {

// What an address gives a route: the parameters of its path and of its query, percent-decoded, by name.
struct route_values {
  std::vector<std::pair<std::string, std::string>> path;
  std::vector<std::pair<std::string, std::string>> query;

  [[nodiscard]] std::string const *find(std::string_view name, bool in_path) const noexcept {
    for (auto const &[key, value] : in_path ? path : query) {
      if (key == name) {
        return &value;
      }
    }

    return nullptr;
  }
};

enum class route_kind : unsigned char { page, redirect, not_found };

// What a route's page is: a component, `component<props>` or `component<>`, or a struct's tag<C>, named by the object.
template <class C>
inline constexpr bool is_page = false;

template <class C>
  requires requires { typename C::props_type; }
inline constexpr bool is_page<C> =
    std::derived_from<C, component_base<typename C::props_type>> && !std::derived_from<C, component<C>>;

template <auto const &Page>
using page_props_t = typename std::remove_cvref_t<decltype(Page)>::props_type;

template <auto const &Page>
inline constexpr bool has_page_props = !std::is_void_v<page_props_t<Page>>;

// What tells a route's page apart, read as the program runs: its table, which a copy of the component shares, and
// which a component declared in a header and defined in a source only has once the program runs.
template <auto const &Page>
[[nodiscard]] component_table const *page_table() noexcept {
  return &Page.table();
}

// A route of a table, its parents before it: its whole path template, its page, what shows it, what tells whether an
// address's values suit its page, and the address of its page for props.
struct route_entry {
  route_kind kind = route_kind::page;
  std::string_view path;
  // A redirect's target, a whole path template whose parameters are its own.
  std::string_view target;
  // Its page's table, for the component the route names.
  component_table const *(*page)() noexcept = nullptr;
  std::size_t parent = static_cast<std::size_t>(-1);
  bool leaf = true;
  element (*show)(route_values const &values) = nullptr;
  bool (*accepts)(route_values const &values) = nullptr;
  std::string (*href)(void const *props) = nullptr;
};

template <std::size_t N>
struct route_table {
  std::array<route_entry, N> entries{};
};

template <http::fixed_string S>
struct stored {
  static constexpr auto value = S;
};

// `path` under `prefix`: a route of the top level is a whole path, one under another adds its own segments to its
// parent's, none for an index route.
template <http::fixed_string Prefix, http::fixed_string Path>
[[nodiscard]] consteval auto joined_path() {
  constexpr std::string_view prefix = Prefix.view();
  constexpr std::string_view path = Path.view();
  constexpr bool top = prefix.empty();
  constexpr std::size_t size = top             ? path.size()
                               : path.empty()  ? prefix.size()
                               : prefix == "/" ? 1 + path.size()
                                               : prefix.size() + 1 + path.size();

  http::fixed_string<size + 1> joined{};
  std::size_t at = 0;
  auto const add = [&](std::string_view part) {
    for (auto const character : part) {
      joined.value[at++] = character;
    }
  };

  if (top) {
    add(path);
  } else if (path.empty()) {
    add(prefix);
  } else {
    add(prefix == "/" ? std::string_view{} : prefix);
    add("/");
    add(path);
  }

  return joined;
}

template <http::fixed_string Prefix, http::fixed_string Path>
struct joined {
  static constexpr auto value = joined_path<Prefix, Path>();
};

// The parameters of a whole path template, in order.
[[nodiscard]] constexpr std::vector<std::string_view> template_params(std::string_view path) {
  std::vector<std::string_view> names;

  for (auto const &segment : http::detail::parse_template(path).segments) {
    if (segment.kind != http::detail::segment_kind::literal) {
      names.push_back(segment.text);
    }
  }

  return names;
}

// Whether the whole path template `path` has the parameter `name`.
[[nodiscard]] constexpr bool has_param(std::string_view path, std::string_view name) {
  for (auto const param : template_params(path)) {
    if (param == name) {
      return true;
    }
  }

  return false;
}

template <auto Field>
inline constexpr bool named = is_named_field<std::remove_cvref_t<decltype(Field)>>;

template <auto Field>
using field_member = member_traits<std::remove_cvref_t<decltype(member_of<Field>())>>;

// The number of the fields a route names by the rank of its path's {parameters}, those named left out.
template <auto... Fields>
inline constexpr std::size_t path_field_count = (std::size_t{0} + ... + (named<Fields> ? 0 : 1));

// The rank among those of the path, by rank, of the field at `Index`.
template <std::size_t Index, auto... Fields>
[[nodiscard]] consteval std::size_t rank_of() {
  std::size_t rank = 0;
  std::size_t at = 0;

  ((rank += (at++ < Index && !named<Fields>) ? 1 : 0), ...);
  return rank;
}

// What the address names the field at `Index` by, in a route of the whole template `Full`: its parameter's name, by
// its rank, or the name it is given.
template <http::fixed_string Full, std::size_t Index, auto... Fields>
[[nodiscard]] consteval std::string_view name_of() {
  constexpr auto field = field_at<Index, Fields...>;

  if constexpr (named<field>) {
    return std::remove_cvref_t<decltype(field)>::name.view();
  } else {
    return template_params(Full.view())[rank_of<Index, Fields...>()];
  }
}

// Whether the field at `Index` is read from the path: by its rank, or named with from_path.
template <std::size_t Index, auto... Fields>
[[nodiscard]] consteval bool from_path_at() {
  constexpr auto field = field_at<Index, Fields...>;

  if constexpr (named<field>) {
    return std::remove_cvref_t<decltype(field)>::in_path;
  } else {
    return true;
  }
}

// Whether no two fields a route names fill the same member, or read the same name of the query or of the path, by
// rank or by name.
template <http::fixed_string Full, auto... Fields>
[[nodiscard]] consteval bool distinct_fields() {
  constexpr std::size_t count = sizeof...(Fields);

  return []<std::size_t... I>(std::index_sequence<I...>) {
    auto const differ = []<std::size_t A, std::size_t B>() {
      if constexpr (A >= B) {
        return true;
      } else {
        constexpr auto left = field_at<A, Fields...>;
        constexpr auto right = field_at<B, Fields...>;
        bool same_member = false;

        if constexpr (std::is_same_v<decltype(member_of<left>()), decltype(member_of<right>())>) {
          same_member = member_of<left>() == member_of<right>();
        }

        bool const same_name = from_path_at<A, Fields...>() == from_path_at<B, Fields...>() &&
                               name_of<Full, A, Fields...>() == name_of<Full, B, Fields...>();

        return !same_member && !same_name;
      }
    };
    auto const row = [&]<std::size_t A>() {
      return [&]<std::size_t... J>(std::index_sequence<J...>) {
        return (differ.template operator()<A, J>() && ...);
      }(std::make_index_sequence<count>{});
    };

    return (row.template operator()<I>() && ...);
  }(std::make_index_sequence<count>{});
}

// The checks of the fields a route of the whole template `Full` names for `Page`, one message a way to be wrong.
template <http::fixed_string Full, auto const &Page, auto... Fields>
consteval void check_fields(page_fields<Fields...> /*fields*/) {
  static_assert(
      (field_member<Fields>::valid && ...),
      "quoll::ui: a route names the fields of its page's props, `&props::id`, `from_path<\"id\", &props::id>` or "
      "`from_query<\"q\", &props::q>`"
  );

  if constexpr ((field_member<Fields>::valid && ...)) {
    if constexpr (has_page_props<Page>) {
      static_assert(
          std::is_class_v<page_props_t<Page>> && !is_parameters<page_props_t<Page>>,
          "quoll::ui: a routed page takes one struct of props, whose fields its route names"
      );
      static_assert(
          (std::is_base_of_v<typename field_member<Fields>::owner, page_props_t<Page>> && ...),
          "quoll::ui: a route names fields of its own page's props"
      );
      static_assert(
          (route_field<typename field_member<Fields>::field> && ...),
          "quoll::ui: a field a route fills is a number, a bool, a string, or an optional of one"
      );
      static_assert(
          std::default_initializable<page_props_t<Page>>,
          "quoll::ui: a routed page's props are made with nothing, what the address lacks left as made"
      );
      static_assert(
          path_field_count<Fields...> != template_params(Full.view()).size() || distinct_fields<Full, Fields...>(),
          "quoll::ui: a route names each field once, and each name of its address once"
      );
    } else {
      static_assert(sizeof...(Fields) == 0, "quoll::ui: a page with no props has no fields to fill");
    }

    static_assert(
        path_field_count<Fields...> == template_params(Full.view()).size(),
        "quoll::ui: a route names one field of its page's props for each {parameter} of its path, in order"
    );
  }
}

// The checks of a route of `Path`, its own as written, whole as `Full`, for `Page` and the fields it names, one
// message a way to be wrong.
template <http::fixed_string Path, http::fixed_string Full, auto const &Page, bool Top, class Fields>
consteval void check_route() {
  constexpr auto parsed_error = http::detail::parse_template(Full.view()).error;
  constexpr bool typed = [] {
    for (auto const &segment : http::detail::parse_template(Full.view()).segments) {
      if (segment.kind == http::detail::segment_kind::param && !segment.type.empty()) {
        return true;
      }
    }

    return false;
  }();

  static_assert(
      Top == (!Path.view().empty() && Path.view().front() == '/'),
      "quoll::ui: a route of the top level starts with '/', one under another does not"
  );
  static_assert(
      parsed_error == http::detail::template_error::none,
      "quoll::ui: a route's path is a valid template: whole segments, {name} parameters, balanced braces"
  );
  static_assert(
      !typed, "quoll::ui: a route parameter takes its type from its page's props: write {id}, not {id:int64}"
  );
  static_assert(
      Full.view() == "/" || Full.view().back() != '/',
      "quoll::ui: a route's path does not end with '/', which an address loses as it is matched"
  );

  if constexpr (parsed_error == http::detail::template_error::none) {
    check_fields<Full, Page>(Fields{});
  }
}

// Fills the field at `Index` from the address, or only tells whether it suits it with `PathOnly`, which leaves the
// query's alone: false when the path has it and it is no such value, or when a field of the route's own path lacks it.
template <bool PathOnly, http::fixed_string Full, std::size_t Index, auto... Fields, class Props>
[[nodiscard]] bool fill_field(Props &props, route_values const &values) {
  constexpr auto field = field_at<Index, Fields...>;
  constexpr std::string_view name = name_of<Full, Index, Fields...>();
  constexpr bool in_path = from_path_at<Index, Fields...>();

  if constexpr (PathOnly && !in_path) {
    return true;
  }

  auto const *const text = values.find(name, in_path);

  if (text == nullptr) {
    return named<field>;
  }

  // A query's value that is no such value leaves the field as made; one of the path matches no route.
  return parse_route_field(*text, props.*member_of<field>()) || !in_path;
}

// The props of `Page` from what an address gives, the fields its route names, each one left as it was made when the
// address does not have it, the query's left alone with `PathOnly`; false when a parameter of the path is no such
// value.
template <bool PathOnly, http::fixed_string Full, auto const &Page, auto... Fields>
[[nodiscard]] bool
fill_props(page_props_t<Page> &props, route_values const &values, page_fields<Fields...> /*fields*/) {
  return [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    bool suits = true;

    ((suits = fill_field<PathOnly, Full, Index, Fields...>(props, values) && suits), ...);
    return suits;
  }(std::make_index_sequence<sizeof...(Fields)>{});
}

template <http::fixed_string Full, auto const &Page, class Fields>
[[nodiscard]] element show_page(route_values const &values) {
  if constexpr (has_page_props<Page>) {
    page_props_t<Page> props{};

    static_cast<void>(fill_props<false, Full, Page>(props, values, Fields{}));
    return Page(std::move(props));
  } else {
    return Page();
  }
}

template <http::fixed_string Full, auto const &Page, class Fields>
[[nodiscard]] bool accepts_page(route_values const &values) {
  if constexpr (has_page_props<Page>) {
    page_props_t<Page> props{};

    return fill_props<true, Full, Page>(props, values, Fields{});
  } else {
    return true;
  }
}

// The address of `Page` for `props`, by the whole template `Full`: each parameter from the field its route names for
// it, and the query's fields set otherwise than as a value-made props has them. A field named from the path is the
// parameter of a route under this one, which its own address writes; a field the route does not name is in none.
template <http::fixed_string Full, auto const &Page, auto... Fields>
[[nodiscard]] std::string href_with(void const *given, page_fields<Fields...> /*fields*/) {
  std::string href;

  if constexpr (has_page_props<Page>) {
    auto const &props = *static_cast<page_props_t<Page> const *>(given);
    page_props_t<Page> const made{};
    std::array<std::string, path_field_count<Fields...>> path_texts{};
    std::string query;
    char separator = '?';

    auto const write = [&]<std::size_t Index>() {
      constexpr auto field = field_at<Index, Fields...>;
      auto const &value = props.*member_of<field>();

      if constexpr (!named<field>) {
        path_texts[rank_of<Index, Fields...>()] = format_route_field(value).value_or(std::string{});
      } else if constexpr (!std::remove_cvref_t<decltype(field)>::in_path) {
        if (value == made.*member_of<field>()) {
          return;
        }

        if (auto const text = format_route_field(value)) {
          query += separator;
          query += http::detail::percent_encode(name_of<Full, Index, Fields...>());
          query += '=';
          query += http::detail::percent_encode(*text);
          separator = '&';
        }
      }
    };

    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (write.template operator()<Index>(), ...);
    }(std::make_index_sequence<sizeof...(Fields)>{});

    std::size_t rank = 0;

    for (auto const &segment : http::detail::parse_template(Full.view()).segments) {
      href += '/';

      if (segment.kind == http::detail::segment_kind::literal) {
        href += segment.text;
      } else {
        href += http::detail::percent_encode(
            path_texts[rank++],
            segment.kind == http::detail::segment_kind::rest ? std::string_view{"/"} : std::string_view{}
        );
      }
    }

    href += query;
  } else {
    for (auto const &segment : http::detail::parse_template(Full.view()).segments) {
      href += '/';
      href += segment.text;
    }
  }

  return href.empty() || href.front() == '?' ? "/" + href : href;
}

template <http::fixed_string Full, auto const &Page, class Fields>
[[nodiscard]] std::string href_of(void const *given) {
  return href_with<Full, Page>(given, Fields{});
}

template <class Node>
struct route_count : std::integral_constant<std::size_t, 1> {};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_ROUTING_ROUTE_TABLE_HPP
