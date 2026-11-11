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

#ifndef QUOLL_UI_ELEMENT_HPP
#define QUOLL_UI_ELEMENT_HPP

#include <quoll/async/task.hpp>
#include <quoll/ui/detail/held_view.hpp>
#include <quoll/ui/node_id.hpp>

#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace quoll::ui::detail {

struct component_spec;

} // namespace quoll::ui::detail

namespace quoll::ui {

/**
 * @brief What an event runs: given the value of the node, for an event that has one. on() makes one from a function; a
 * coroutine written directly as one takes a `std::string`, as a reference dies with this call.
 */
using event_handler = std::function<async::task<>(std::string value)>;

/** @brief An event of an element, and what it runs. */
struct event_binding {
  std::string name;
  // The component whose view made it, whose task its handler runs in, none for the app's own, and the props of that
  // view, which its handler may read: kept until the handler's task ends, and declared before the handler, so that what
  // it captures goes while they are still there. Quoll sets it once, on a binding fresh from a view, and never assigns
  // a binding over another, which would release them first: it replaces whole lists of elements.
  detail::held_view held{};
  event_handler handler;
};

/**
 * @brief A piece of interface: what view() returns. A value, cheap to build and moved around; made by the functions of
 * tags.hpp and completed with the decorators of decorators.hpp.
 */
struct element {
  enum class kind {
    node,
    text,
    // Stands for its children, in its parent: what list() and when() make.
    fragment,
    // A component of its own, with its props: component<C>() makes one, which the app replaces with its view.
    component,
  };

  kind type = kind::node;
  std::string tag{};
  // Of a text.
  std::string content{};
  std::string classes{};
  std::string key{};
  // The value of an input.
  std::optional<std::string> value{};
  std::map<std::string, std::string> attributes{};
  std::vector<event_binding> events{};
  std::vector<element> children{};
  // Of a component.
  std::shared_ptr<detail::component_spec const> component{};
  // Given when the element is shown.
  node_id id = no_node;
};

namespace detail {

// An element in the braces of a parent. An initializer_list only hands out const elements: `mutable` lets them be moved
// rather than copied, each one with all its children.
struct child {
  // Implicit: what the braces of `div({...})` hold.
  child(element value) noexcept : held(std::move(value)) {}

  mutable element held;
};

void append_child(std::vector<element> &children, element &&value);

[[nodiscard]] std::vector<element> to_children(std::initializer_list<child> values);

} // namespace detail

} // namespace quoll::ui

#endif // QUOLL_UI_ELEMENT_HPP
