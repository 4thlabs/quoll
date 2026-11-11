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

#ifndef QUOLL_UI_DETAIL_RECONCILER_HPP
#define QUOLL_UI_DETAIL_RECONCILER_HPP

#include <quoll/ui/detail/widget_spec.hpp>
#include <quoll/ui/element.hpp>
#include <quoll/ui/node_id.hpp>
#include <quoll/ui/renderer_ref.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace quoll::ui::detail {

// Turns what is shown into what should be, with few operations, and gives each new element its node.
class reconciler {
public:
  reconciler(renderer_ref renderer, node_id &last_id) noexcept : renderer_(renderer), last_id_(&last_id) {}

  // Keeps the nodes of the children that have the same key (their position, without one) and the same tag. Throws
  // std::logic_error for a widget with children, or placed through a renderer that shows none, before making it.
  void apply(node_id parent, std::vector<element> &before, std::vector<element> &after);

  // The widgets it made, which its caller mounts once the render shows, as a module may emit as it mounts.
  [[nodiscard]] std::vector<std::pair<node_id, std::shared_ptr<widget_spec const>>> take_mounts() noexcept {
    return std::move(to_mount_);
  }

private:
  void children(node_id parent, std::vector<element> &before, std::vector<element> &after);
  void create(element &value);
  void patch(element &before, element &after);
  void patch_attributes(element const &before, element const &after);
  void patch_events(element const &before, element const &after);
  void remove(element const &value);

  renderer_ref renderer_;
  node_id *last_id_;
  // The widgets made, to mount once the render has placed every node.
  std::vector<std::pair<node_id, std::shared_ptr<widget_spec const>>> to_mount_;
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_RECONCILER_HPP
