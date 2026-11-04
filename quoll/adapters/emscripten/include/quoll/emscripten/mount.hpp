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

#ifndef QUOLL_EMSCRIPTEN_MOUNT_HPP
#define QUOLL_EMSCRIPTEN_MOUNT_HPP

#include <quoll/emscripten/dom_renderer.hpp>
#include <quoll/emscripten/executor.hpp>
#include <quoll/ui/app.hpp>
#include <quoll/ui/component.hpp>
#include <quoll/ui/detail/hot.hpp>
#include <quoll/ui/node_id.hpp>

#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace quoll::emscripten {

/**
 * @brief Shows a component in the page, in the element found by `selector`, for as long as the page lives: what the
 * main() of an interface calls.
 *
 * @code
 * int main() { quoll::emscripten::mount<notes_page>("#app"); }
 * @endcode
 *
 * Returning from main() leaves the event loop running, and the app with it.
 *
 * @throws std::invalid_argument when nothing matches `selector`.
 * @throws std::logic_error when this component is already mounted: one app per component type.
 */
template <ui::Component C>
C &mount(std::string_view selector = "#app", ui::app_options options = {}) {
  using app_type = ui::app<C, dom_renderer>;

  // For as long as the page: a page has no end to wait for.
  static std::optional<app_type> instance;

  if (instance) {
    throw std::logic_error{"quoll::emscripten::mount: this component is already mounted"};
  }

  instance.emplace(executor{}, dom_renderer{selector}, std::move(options));

  auto const dispatch = [](void *owner, ui::node_id id, char const *name, char const *value) {
    static_cast<app_type *>(owner)->dispatch(id, name, value);
  };

  instance->renderer().on_event(dispatch, &*instance);

#if defined(QUOLL_UI_HOT)
  detail::quoll_emscripten_ui_hot_install([](char const *file, unsigned line, unsigned column, char const *value) {
    ui::detail::hot_replace(file, line, column, value);
  });
#endif

  instance->start();

  return instance->component();
}

} // namespace quoll::emscripten

#endif // QUOLL_EMSCRIPTEN_MOUNT_HPP
