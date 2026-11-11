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

#ifndef QUOLL_UI_WIDGET_HPP
#define QUOLL_UI_WIDGET_HPP

#include <quoll/ui/detail/widget_spec.hpp>
#include <quoll/ui/element.hpp>

#include <glaze/json/write.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace quoll::ui {

/**
 * @brief A JavaScript library shown in a node of the view, as chart.js in a `canvas`: declared once, with the module
 * that mounts it and its props, then placed in a view as an element.
 *
 * @code
 * struct chart_props {
 *   std::vector<std::string> labels;
 *   std::vector<double> values;
 * };
 *
 * inline constexpr auto line_chart = widget<chart_props>("./widgets/line_chart.js", "canvas");
 *
 * // In a view:
 * auto const select = [this](point_selected event) {
 *   selected = event.index;
 * };
 *
 * return line_chart({.labels = months, .values = *sales}) | tw("h-64") | on("point_selected", select);
 * @endcode
 *
 * The module exports `mount(node, props, emit)`, which returns `update(props)` and `unmount()`. The app leaves the
 * node's children to it: it calls `mount` once the node is in place, `update` when the props, written as JSON, change,
 * and `unmount` before the node goes. What it emits is an event of the node, its value as JSON (see on() of
 * json_event.hpp). The element takes classes, attributes and events, and no children.
 *
 * Thread safety: a value; the element it makes is shown on its app's executor.
 */
template <class Props>
class widget {
public:
  /**
   * @brief The widget of `module`, the path its bundler imports, shown in a node of `tag`: literals, which it refers
   * to, and which the bundler reads in the source to load the module on demand.
   */
  consteval widget(std::string_view module, std::string_view tag) noexcept : module_(module), tag_(tag) {}

  /** @brief An element showing it, with `props`, written as JSON. */
  [[nodiscard]] element operator()(Props const &props) const {
    std::string written;

    if (auto const failed = glz::write_json(props, written)) {
      throw std::invalid_argument{"quoll::ui: the props of a widget cannot be written as JSON"};
    }

    element made{.tag = std::string{tag_}};

    made.type = element::kind::widget;
    made.widget = std::make_shared<detail::widget_spec const>(
        detail::widget_spec{.module = std::string{module_}, .props = std::move(written)});
    return made;
  }

  /** @brief The path of its module. */
  [[nodiscard]] constexpr std::string_view module() const noexcept { return module_; }

private:
  std::string_view module_;
  std::string_view tag_;
};

} // namespace quoll::ui

#endif // QUOLL_UI_WIDGET_HPP
