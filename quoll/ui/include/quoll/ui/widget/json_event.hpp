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

#ifndef QUOLL_UI_WIDGET_JSON_EVENT_HPP
#define QUOLL_UI_WIDGET_JSON_EVENT_HPP

#include <quoll/async/task.hpp>
#include <quoll/ui/view/decorators.hpp>
#include <quoll/ui/widget/json_event_function.hpp>

#include <glaze/json/read.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::ui {

namespace detail {

template <JsonEventFunction F>
[[nodiscard]] event_handler to_json_handler(F function) {
  return [function = std::move(function)](std::string value) mutable -> async::task<> {
    one_parameter_t<F> event{};

    // Fields it does not declare are left: a library's events carry more than one wants.
    if (auto const failed = glz::read<glz::opts{.error_on_unknown_keys = false}>(event, value)) {
      throw std::invalid_argument{"quoll::ui: the value of an event is not the JSON its function takes"};
    }

    if constexpr (std::is_void_v<std::invoke_result_t<F &, one_parameter_t<F> &>>) {
      std::invoke(function, event);
    } else {
      co_await std::invoke(function, event);
    }

    co_return;
  };
}

} // namespace detail

/**
 * @brief Runs `function` on the event `name` of the element, its value, JSON, read into the function's parameter: what
 * a widget emits.
 *
 * @code
 * struct point_selected {
 *   int index;
 * };
 *
 * auto const select = [&selected](point_selected event) {
 *   selected = event.index;
 * };
 *
 * return line_chart({.labels = months, .values = *sales}) | on("point_selected", select);
 * @endcode
 *
 * Fields the parameter does not declare are left. A value that is not that JSON throws `std::invalid_argument` in the
 * event's task, which goes to the app's on_error.
 * Otherwise as the other on().
 */
template <JsonEventFunction F>
[[nodiscard]] auto on(std::string_view name, F function) {
  return decorator([name = std::string{name}, handler = detail::to_json_handler(std::move(function))](element &target) {
    target.events.push_back({.name = name, .handler = handler});
  });
}

} // namespace quoll::ui

#endif // QUOLL_UI_WIDGET_JSON_EVENT_HPP
