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

#ifndef QUOLL_UI_RENDERER_HPP
#define QUOLL_UI_RENDERER_HPP

#include <quoll/ui/node_id.hpp>

#include <string_view>

namespace quoll::ui {

/**
 * @brief Applies the changes of an interface to what shows it: the DOM in a browser, a tree in memory in a test.
 *
 * Every operation names its nodes by id. `insert(parent, child, before)` places a node, or moves one already placed,
 * before `before`, or at the end of `parent` with no_node; the interface's own parent is no_node as well. `remove(id)`
 * forgets a node: its descendants are removed first. `set_value` and `set_checked` set the properties of an input,
 * what it holds and whether it is checked, as the browser shows them. `listen(id, name)` asks for the events `name` of
 * a node, reported to app::dispatch() with a value: for a key event the key, after the modifiers held, as
 * "Shift+Enter"; for a checkbox "true" or "false"; for any other what the node holds. A form never leaves the page.
 */
template <class R>
concept Renderer = requires(R &renderer, node_id id, std::string_view text, bool flag) {
  renderer.create_element(id, text);
  renderer.create_text(id, text);
  renderer.set_text(id, text);
  renderer.set_class(id, text);
  renderer.set_attribute(id, text, text);
  renderer.remove_attribute(id, text);
  renderer.set_value(id, text);
  renderer.set_checked(id, flag);
  renderer.listen(id, text);
  renderer.unlisten(id, text);
  renderer.insert(id, id, id);
  renderer.remove(id);
};

} // namespace quoll::ui

#endif // QUOLL_UI_RENDERER_HPP
