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

#ifndef QUOLL_UI_DETAIL_EXPAND_HPP
#define QUOLL_UI_DETAIL_EXPAND_HPP

#include <quoll/ui/detail/held_view.hpp>
#include <quoll/ui/detail/instance.hpp>
#include <quoll/ui/detail/view_context.hpp>
#include <quoll/ui/element.hpp>
#include <quoll/ui/state.hpp>

#include <cstdint>
#include <memory>
#include <stop_token>
#include <utility>
#include <vector>

namespace quoll::ui::detail {

// What an expansion makes components with: the app their callbacks call back, none for a view written once, the app's
// stop, which theirs follow, the link of their states, none for a
// view written once (to_html), and where what their unmounted() throws goes. Then what it found, for commit(): the
// components it made, to mount once shown, and what changes for those it kept.
struct expansion {
  std::shared_ptr<app_link> app{};
  // Of the app's hot reloads: a last view from another is not given again.
  std::uint64_t epoch = 0;
  std::stop_token app_stop;
  state_link link{};
  failure_sink sink{};
  std::vector<std::shared_ptr<instance>> made{};
  std::vector<std::shared_ptr<instance>> now{};

  // A view that ran: for a component kept, what commit() gives it, or discard() keeps of it.
  struct view_record {
    std::shared_ptr<instance> owner;
    std::shared_ptr<void const> props;
    std::shared_ptr<void const> own;
    std::shared_ptr<void const> placed_by;
    element view;
    std::uint64_t version;
    std::uint64_t epoch;
  };

  std::vector<view_record> views{};
  std::vector<std::pair<std::shared_ptr<instance>, std::vector<std::shared_ptr<instance>>>> children{};
  std::vector<std::shared_ptr<instance>> leaving{};
};

// Replaces each component of `view` by its view, the components of that one by theirs in turn, so that what is left
// is elements a renderer shows. A component at the same place as one of `shown`, the components the last view had
// there, of the same type and the same key, or the same rank among those of its type with none, keeps its instance,
// to get its new props; one that has none any more is to be unmounted. Each handler of the elements a component's view
// made runs in that component's task, holding the props of that view, those of the app's own in the app's. The nodes of
// a component's view take its key, or its type and rank, ahead of their own, so that they move with it.
//
// Changes no component: commit() applies what it found, once what it made shows, and links the states of what it made,
// or discard() drops it. What it throws leaves nothing made behind. A view with no component, after one with none, has
// nothing to expand: has_component() tells.
//
// Throws std::logic_error for a component element decorated otherwise than with a key: what a component shows is its
// view's, decorated there; and for two components of one type with one key in a view, even in two lists of it, which
// could not tell them apart.
void expand(element &view, std::vector<std::shared_ptr<instance>> const &shown, expansion &context);

// Applies what expand() found: `shown` becomes the components of the view, the states of those made are linked, the
// props and the components of those kept are replaced, and those that go are unmounted, what their unmounted() throws
// going to the sink.
void commit(expansion &context, std::vector<std::shared_ptr<instance>> &shown) noexcept;

// Drops what expand() found: the components it made go, never mounted, and those kept whose view returned hold the
// props it was given, which it may refer to.
void discard(expansion &context) noexcept;

// Whether `value` holds a component, which expand() replaces.
[[nodiscard]] bool has_component(element const &value) noexcept;

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_EXPAND_HPP
