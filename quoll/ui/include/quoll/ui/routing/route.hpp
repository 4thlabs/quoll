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

#ifndef QUOLL_UI_ROUTING_ROUTE_HPP
#define QUOLL_UI_ROUTING_ROUTE_HPP

#include <quoll/http/fixed_string.hpp>
#include <quoll/ui/component/component.hpp>
#include <quoll/ui/detail/routing/route_table.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace quoll::ui {

namespace detail {

template <http::fixed_string Path, auto const &Page, class Fields, class... Children>
struct route_node {
  std::tuple<Children...> children;
};

template <http::fixed_string From, http::fixed_string To>
struct redirect_node {};

template <auto const &Page, class Fields>
struct not_found_node {};

// Whether the whole template `Full`, or that of a route under it, has the parameter `name`.
template <http::fixed_string Full, class Node>
struct subtree_has {
  static constexpr bool find(std::string_view /*name*/) { return false; }
};

template <http::fixed_string Full, http::fixed_string Path, auto const &Page, class Fields, class... Children>
struct subtree_has<Full, route_node<Path, Page, Fields, Children...>> {
  static constexpr auto const &full = joined<Full, Path>::value;

  static constexpr bool find(std::string_view name) {
    return has_param(full.view(), name) || (subtree_has<full, Children>::find(name) || ...);
  }
};

// Whether each field the route of `Full` names from the path is a parameter of its path or of one under it.
template <http::fixed_string Full, class... Children, auto... Fields>
[[nodiscard]] consteval bool path_names_found(page_fields<Fields...> /*fields*/) {
  auto const found = [](std::string_view name) {
    return has_param(Full.view(), name) || (subtree_has<Full, Children>::find(name) || ...);
  };

  [[maybe_unused]] auto const check = [&]<auto Field>() {
    if constexpr (named<Field>) {
      using field_type = std::remove_cvref_t<decltype(Field)>;

      return !field_type::in_path || found(field_type::name.view());
    } else {
      return true;
    }
  };

  return (check.template operator()<Fields>() && ...);
}

template <http::fixed_string Path, auto const &Page, class Fields, class... Children>
struct route_count<route_node<Path, Page, Fields, Children...>>
    : std::integral_constant<std::size_t, (1 + ... + route_count<Children>::value)> {};

template <http::fixed_string Prefix, class Node, std::size_t N>
struct flatten;

template <
    http::fixed_string Prefix, http::fixed_string Path, auto const &Page, class Fields, class... Children,
    std::size_t N>
struct flatten<Prefix, route_node<Path, Page, Fields, Children...>, N> {
  static constexpr auto const &full = joined<Prefix, Path>::value;

  static consteval void into(std::array<route_entry, N> &entries, std::size_t &at, std::size_t parent) {
    check_route<Path, full, Page, Prefix.view().empty(), Fields>();
    static_assert(
        path_names_found<full, Children...>(Fields{}),
        "quoll::ui: a field named from the path names a parameter of its route's path, or of one under it"
    );

    auto const index = at++;

    entries[index] =
        {.kind = route_kind::page,
         .path = full.view(),
         .target = {},
         .page = &page_table<Page>,
         .parent = parent,
         .leaf = sizeof...(Children) == 0,
         .show = &show_page<full, Page, Fields>,
         .accepts = &accepts_page<full, Page, Fields>,
         .href = &href_of<full, Page, Fields>};

    (flatten<full, Children, N>::into(entries, at, index), ...);
  }
};

template <http::fixed_string Prefix, http::fixed_string From, http::fixed_string To, std::size_t N>
struct flatten<Prefix, redirect_node<From, To>, N> {
  static constexpr auto const &full = joined<Prefix, From>::value;

  static consteval void into(std::array<route_entry, N> &entries, std::size_t &at, std::size_t parent) {
    static_assert(
        http::detail::parse_template(full.view()).error == http::detail::template_error::none &&
            http::detail::parse_template(To.view()).error == http::detail::template_error::none,
        "quoll::ui: a redirect goes from a valid template to a valid template"
    );
    static_assert(
        http::detail::parse_template(full.view()).query.empty() &&
            http::detail::parse_template(To.view()).query.empty(),
        "quoll::ui: a redirect's paths have no query"
    );
    static_assert(
        (full.view() == "/" || full.view().back() != '/') && (To.view() == "/" || To.view().back() != '/'),
        "quoll::ui: a redirect's paths do not end with '/', which an address loses as it is matched"
    );
    static_assert(
        [] {
          for (auto const name : template_params(To.view())) {
            if (!has_param(full.view(), name)) {
              return false;
            }
          }

          return true;
        }(),
        "quoll::ui: each parameter of a redirect's target is one of its path's"
    );

    entries[at++] = {
        .kind = route_kind::redirect,
        .path = full.view(),
        .target = stored<To>::value.view(),
        .page = nullptr,
        .parent = parent,
        .leaf = true,
        .show = nullptr,
        .accepts = nullptr,
        .href = nullptr
    };
  }
};

template <http::fixed_string Prefix, auto const &Page, class Fields, std::size_t N>
struct flatten<Prefix, not_found_node<Page, Fields>, N> {
  static consteval void into(std::array<route_entry, N> &entries, std::size_t &at, std::size_t parent) {
    static_assert(Prefix.view().empty(), "quoll::ui: a not_found route is of the top level");
    check_fields<"", Page>(Fields{});
    static_assert(
        path_names_found<"">(Fields{}), "quoll::ui: a not_found route has no path, its fields come from the query"
    );

    entries[at++] = {
        .kind = route_kind::not_found,
        .path = {},
        .target = {},
        .page = &page_table<Page>,
        .parent = parent,
        .leaf = true,
        .show = &show_page<"", Page, Fields>,
        .accepts = &accepts_page<"", Page, Fields>,
        .href = nullptr
    };
  }
};

} // namespace detail

/**
 * @brief A field of a page's props that the query of its address fills, by its name: `from_query<"q",
 * &list_props::q>` in a route, as `?q=tea`. See route.
 */
template <http::fixed_string Name, auto Member>
inline constexpr detail::named_field<Name, Member, false> from_query{};

/**
 * @brief A field of a page's props that a parameter of the path matched fills, by its name, that of the route or of one
 * under it: `from_path<"id", &layout_props::id>` gives a layout the `{id}` of the page it shows. See route.
 */
template <http::fixed_string Name, auto Member>
inline constexpr detail::named_field<Name, Member, true> from_path{};

/**
 * @brief A route: the page shown at the addresses its path matches, with the routes under it.
 *
 * @code
 * inline constexpr auto routes = ui::routes(
 *     ui::route<"/", home_page>(),
 *     ui::route<"/products", products_layout>(
 *         ui::route<"", product_list, ui::from_query<"q", &product_list_props::q>>(),
 *         ui::route<"{id}", product_page, &product_page_props::id>()),
 *     ui::redirect<"/items/{id}", "/products/{id}">(),
 *     ui::not_found<missing_page>());
 * @endcode
 *
 * Its page is a component, named by the object, `component<props>` or `component<>`, as declared in a header. Its
 * path is a template, checked at compile time: a route of the top level starts with '/', one under another adds
 * its own segments, none for the index route of its parent. After its page, a route names the fields of its page's
 * props the address fills: one member pointer for each `{parameter}` of its whole path, in order, whose type gives
 * the parameter's (`{name:path}` takes the rest of the path, a string). Then, by name, `from_query<"q", &props::q>`
 * for a field the query gives, `?q=tea`, and `from_path<"id", &props::id>` for one a parameter of the path matched
 * gives, as a layout's of the page it shows; each left as made when the address lacks it. A field not named stays as
 * made. A route with routes under it is a layout: its page shows the one of the address in its outlet().
 */
template <http::fixed_string Path, auto const &Page, auto... Fields, class... Children>
[[nodiscard]] consteval detail::route_node<Path, Page, detail::page_fields<Fields...>, Children...>
route(Children... children) {
  static_assert(detail::is_page<std::remove_cvref_t<decltype(Page)>>, "quoll::ui: a route's page is a component");

  return {.children = {children...}};
}

/** @brief Goes from `From` to `To` as the address matches it, its parameters given by name, the history replaced. */
template <http::fixed_string From, http::fixed_string To>
[[nodiscard]] consteval detail::redirect_node<From, To> redirect() {
  return {};
}

/**
 * @brief The page shown when no route matches the address, of the top level, the fields of its props named as a
 * route's from its query: `not_found<missing_page, from_query<"from", &missing_props::from>>()`.
 */
template <auto const &Page, auto... Fields>
[[nodiscard]] consteval detail::not_found_node<Page, detail::page_fields<Fields...>> not_found() {
  static_assert(detail::is_page<std::remove_cvref_t<decltype(Page)>>, "quoll::ui: a route's page is a component");

  return {};
}

/** @brief The routes of an app, in order: the first that matches an address wins. See route. */
template <class... Nodes>
[[nodiscard]] consteval auto routes(Nodes...) {
  constexpr std::size_t size = (0 + ... + detail::route_count<Nodes>::value);

  detail::route_table<size> table{};
  std::size_t at = 0;

  (detail::flatten<"", Nodes, size>::into(table.entries, at, static_cast<std::size_t>(-1)), ...);
  return table;
}

} // namespace quoll::ui

#endif // QUOLL_UI_ROUTING_ROUTE_HPP
