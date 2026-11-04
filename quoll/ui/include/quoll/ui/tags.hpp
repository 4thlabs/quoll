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

#ifndef QUOLL_UI_TAGS_HPP
#define QUOLL_UI_TAGS_HPP

#include <quoll/ui/element.hpp>
#include <quoll/ui/state.hpp>

#include <concepts>
#include <initializer_list>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/** @brief Some text. */
[[nodiscard]] inline element text(std::string_view content) {
  return {.type = element::kind::text, .content = std::string{content}};
}

/** @brief An element named `tag`, holding `children`: for what has no function of its own. */
[[nodiscard]] inline element node(std::string_view tag, std::initializer_list<detail::child> children = {}) {
  return {.tag = std::string{tag}, .children = detail::to_children(children)};
}

namespace detail {

// What `div`, `button`... are: `div({...})` holds children, `button("Add")` holds a text, `div()` nothing.
struct tag_function {
  std::string_view name;

  [[nodiscard]] element operator()(std::initializer_list<child> children = {}) const { return node(name, children); }

  [[nodiscard]] element operator()(std::string_view content) const { return node(name, {text(content)}); }
};

} // namespace detail

inline constexpr detail::tag_function a{"a"};
inline constexpr detail::tag_function article{"article"};
inline constexpr detail::tag_function button{"button"};
inline constexpr detail::tag_function footer{"footer"};
inline constexpr detail::tag_function form{"form"};
inline constexpr detail::tag_function h1{"h1"};
inline constexpr detail::tag_function h2{"h2"};
inline constexpr detail::tag_function h3{"h3"};
inline constexpr detail::tag_function header{"header"};
inline constexpr detail::tag_function label{"label"};
inline constexpr detail::tag_function li{"li"};
inline constexpr detail::tag_function main_{"main"};
inline constexpr detail::tag_function nav{"nav"};
inline constexpr detail::tag_function ol{"ol"};
inline constexpr detail::tag_function p{"p"};
inline constexpr detail::tag_function section{"section"};
inline constexpr detail::tag_function span{"span"};
inline constexpr detail::tag_function ul{"ul"};

// Functions, not a tag_function: `using namespace quoll::ui;` makes them overloads of the C library's ::div(), where an
// object would be ambiguous with it.
[[nodiscard]] inline element div(std::initializer_list<detail::child> children = {}) { return node("div", children); }

[[nodiscard]] inline element div(std::string_view content) { return node("div", {text(content)}); }

/** @brief An input: its value comes from bind(). */
[[nodiscard]] inline element input() { return node("input"); }

/** @brief What `show` makes of each item of `items`, in place: give each a key(), so that it keeps its node. */
template <std::ranges::input_range R, class F>
  requires std::same_as<std::invoke_result_t<F &, std::ranges::range_reference_t<R>>, element>
[[nodiscard]] element list(R &&items, F show) {
  element all{.type = element::kind::fragment};

  for (auto &&item : items) {
    detail::append_child(all.children, show(item));
  }

  return all;
}

/** @brief list() over the value of a state. */
template <class T, class F>
[[nodiscard]] element list(state<T> const &items, F show) {
  return list(*items, std::move(show));
}

/** @brief What `show` makes when `condition` holds, nothing otherwise. */
template <class F>
  requires std::same_as<std::invoke_result_t<F &>, element>
[[nodiscard]] element when(bool condition, F show) {
  element some{.type = element::kind::fragment};

  if (condition) {
    detail::append_child(some.children, show());
  }

  return some;
}

} // namespace quoll::ui

#endif // QUOLL_UI_TAGS_HPP
