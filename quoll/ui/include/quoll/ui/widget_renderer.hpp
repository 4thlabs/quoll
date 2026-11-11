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

#ifndef QUOLL_UI_WIDGET_RENDERER_HPP
#define QUOLL_UI_WIDGET_RENDERER_HPP

#include <quoll/ui/node_id.hpp>
#include <quoll/ui/renderer.hpp>

#include <string_view>

namespace quoll::ui {

/**
 * @brief A Renderer that shows JavaScript widgets too: `mount_widget(id, module, props)` hands the node `id`, once in
 * place, to the module, with its props as JSON; `update_widget(id, props)` gives it others; `unmount_widget(id)` takes
 * it back, before the node is removed. What the widget emits is reported to app::dispatch(), as an event of its node.
 *
 * A renderer without these shows no widget: an app placing one through it throws `std::logic_error`.
 */
template <class R>
concept WidgetRenderer = Renderer<R> && requires(R &renderer, node_id id, std::string_view text) {
  renderer.mount_widget(id, text, text);
  renderer.update_widget(id, text);
  renderer.unmount_widget(id);
};

} // namespace quoll::ui

#endif // QUOLL_UI_WIDGET_RENDERER_HPP
