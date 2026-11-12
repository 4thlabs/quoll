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

#include <quoll/async/detail/type_id.hpp>
#include <quoll/http/detail/percent.hpp>
#include <quoll/http/detail/route_template.hpp>
#include <quoll/http/fixed_string.hpp>
#include <quoll/ui/component/child_component.hpp>
#include <quoll/ui/component/component.hpp>
#include <quoll/ui/component/component_with_props.hpp>
#include <quoll/ui/detail/routing/route_field.hpp>
#include <quoll/ui/view/element.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
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

// A route of a table, its parents before it: its whole path template, its page, what shows it, what tells whether an
// address's values suit its page, and the address of its page for props.
struct route_entry {
  route_kind kind = route_kind::page;
  std::string_view path;
  // A redirect's target, a whole path template whose parameters are its own.
  std::string_view target;
  async::detail::type_id page = nullptr;
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

// The checks of a route of `Path`, its own as written, whole as `Full`, for `Page`, one message a way to be wrong.
template <http::fixed_string Path, http::fixed_string Full, class Page, bool Top>
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

  if constexpr (ComponentWithProps<Page>) {
    using props = typename Page::props;

    static_assert(
        route_fields_only<props>(),
        "quoll::ui: a page's props come from its route: each field is a number, a bool, a string, or an "
        "optional of one"
    );
    static_assert(
        [] {
          for (auto const name : template_params(Full.view())) {
            if (!has_route_field<props>(name)) {
              return false;
            }
          }

          return true;
        }(),
        "quoll::ui: each parameter of a route's path is a field of its page's props, of the same name"
    );
  } else {
    static_assert(template_params(Full.view()).empty(), "quoll::ui: a page with parameters in its route has props");
  }
}

// The props of `Page` from what an address gives: the path's parameters first, then the query's, each field left as it
// was made when neither has it; false when a path parameter is no such value.
template <class Page>
[[nodiscard]] bool fill_props(typename Page::props &props, route_values const &values) {
  bool suits = true;

  for_each_route_field(props, [&](std::string_view name, auto &field) {
    if (auto const *const text = values.find(name, true)) {
      suits = parse_route_field(*text, field) && suits;
    } else if (auto const *const given = values.find(name, false)) {
      static_cast<void>(parse_route_field(*given, field));
    }
  });

  return suits;
}

template <class Page>
[[nodiscard]] element show_page(route_values const &values) {
  if constexpr (ComponentWithProps<Page>) {
    typename Page::props props{};

    static_cast<void>(fill_props<Page>(props, values));
    return component<Page>(std::move(props));
  } else {
    return component<Page>();
  }
}

template <class Page>
[[nodiscard]] bool accepts_page(route_values const &values) {
  if constexpr (ComponentWithProps<Page>) {
    typename Page::props props{};

    return fill_props<Page>(props, values);
  } else {
    return true;
  }
}

// The address of `Page` for `props`, by the whole template `Full`: its parameters from the fields of their names, the
// other fields set otherwise than as a value-made props are in the query.
template <http::fixed_string Full, class Page>
[[nodiscard]] std::string href_of(void const *given) {
  std::string href;

  if constexpr (ComponentWithProps<Page>) {
    auto const &props = *static_cast<typename Page::props const *>(given);
    typename Page::props const made{};
    auto const params = template_params(Full.view());
    auto const value_of = [&props](std::string_view name) {
      std::optional<std::string> found;

      for_each_route_field(props, [&](std::string_view key, auto const &field) {
        if (key == name) {
          found = format_route_field(field);
        }
      });

      return found.value_or(std::string{});
    };

    for (auto const &segment : http::detail::parse_template(Full.view()).segments) {
      href += '/';

      if (segment.kind == http::detail::segment_kind::literal) {
        href += segment.text;
      } else {
        href += http::detail::percent_encode(
            value_of(segment.text),
            segment.kind == http::detail::segment_kind::rest ? std::string_view{"/"} : std::string_view{}
        );
      }
    }

    // The other fields, set otherwise than as props made with nothing are, in the query.
    char separator = '?';

    for_each_route_field_pair(props, made, [&](std::string_view name, auto const &field, auto const &unset) {
      if (field == unset || std::ranges::find(params, name) != params.end()) {
        return;
      }

      if (auto const text = format_route_field(field)) {
        href += separator;
        href += http::detail::percent_encode(name);
        href += '=';
        href += http::detail::percent_encode(*text);
        separator = '&';
      }
    });
  } else {
    for (auto const &segment : http::detail::parse_template(Full.view()).segments) {
      href += '/';
      href += segment.text;
    }
  }

  return href.empty() ? std::string{"/"} : href;
}

template <class Node>
struct route_count : std::integral_constant<std::size_t, 1> {};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_ROUTING_ROUTE_TABLE_HPP
