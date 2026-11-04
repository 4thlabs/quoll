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

#ifndef QUOLL_EMSCRIPTEN_DOM_RENDERER_HPP
#define QUOLL_EMSCRIPTEN_DOM_RENDERER_HPP

#include <quoll/emscripten/detail/js.hpp>
#include <quoll/ui/node_id.hpp>
#include <quoll/ui/renderer.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace quoll::emscripten {

/**
 * @brief A ui::Renderer showing an interface in the DOM, in the element found by a CSS selector.
 *
 * Each operation is a call into JavaScript. The events asked for by listen() come back through on_event(), on the event
 * loop, with the value of the node (what an input holds) or nothing.
 *
 * Copies refer to the same interface. Thread safety: the thread running the event loop only.
 */
class dom_renderer {
public:
  /** @brief What an event calls: the owner given to on_event(), the node, the event's name and the node's value. */
  using event_function = void (*)(void *owner, ui::node_id id, char const *name, char const *value);

  /** @throws std::invalid_argument when nothing matches `selector`. */
  explicit dom_renderer(std::string_view selector)
      : instance_(detail::quoll_emscripten_ui_create(std::string{selector}.c_str())) {
    if (instance_ == 0) {
      throw std::invalid_argument{"quoll::emscripten::dom_renderer: nothing matches " + std::string{selector}};
    }
  }

  void on_event(event_function function, void *owner) noexcept {
    detail::quoll_emscripten_ui_on_event(instance_, function, owner);
  }

  void create_element(ui::node_id id, std::string_view tag) {
    detail::quoll_emscripten_ui_create_element(instance_, id, tag.data(), tag.size());
  }

  void create_text(ui::node_id id, std::string_view content) {
    detail::quoll_emscripten_ui_create_text(instance_, id, content.data(), content.size());
  }

  void set_text(ui::node_id id, std::string_view content) {
    detail::quoll_emscripten_ui_set_text(instance_, id, content.data(), content.size());
  }

  void set_class(ui::node_id id, std::string_view classes) {
    detail::quoll_emscripten_ui_set_class(instance_, id, classes.data(), classes.size());
  }

  void set_attribute(ui::node_id id, std::string_view name, std::string_view value) {
    detail::quoll_emscripten_ui_set_attribute(instance_, id, name.data(), name.size(), value.data(), value.size());
  }

  void remove_attribute(ui::node_id id, std::string_view name) {
    detail::quoll_emscripten_ui_remove_attribute(instance_, id, name.data(), name.size());
  }

  void set_value(ui::node_id id, std::string_view value) {
    detail::quoll_emscripten_ui_set_value(instance_, id, value.data(), value.size());
  }

  void listen(ui::node_id id, std::string_view name) {
    detail::quoll_emscripten_ui_listen(instance_, id, name.data(), name.size());
  }

  void unlisten(ui::node_id id, std::string_view name) {
    detail::quoll_emscripten_ui_unlisten(instance_, id, name.data(), name.size());
  }

  void insert(ui::node_id parent, ui::node_id child, ui::node_id before) {
    detail::quoll_emscripten_ui_insert(instance_, parent, child, before);
  }

  void remove(ui::node_id id) { detail::quoll_emscripten_ui_remove(instance_, id); }

  /** @brief Takes everything it showed out of the page, and its listeners. Nothing can be called afterwards. */
  void close() { detail::quoll_emscripten_ui_close(instance_); }

private:
  int instance_;
};

static_assert(ui::Renderer<dom_renderer>);

} // namespace quoll::emscripten

#endif // QUOLL_EMSCRIPTEN_DOM_RENDERER_HPP
