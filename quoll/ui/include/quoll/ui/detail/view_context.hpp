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

#ifndef QUOLL_UI_DETAIL_VIEW_CONTEXT_HPP
#define QUOLL_UI_DETAIL_VIEW_CONTEXT_HPP

#include <quoll/async/task.hpp>
#include <quoll/ui/detail/held_view.hpp>

#include <memory>

namespace quoll::ui::detail {

class app_core;

// An app as long as it lives: what outlives it, a callback kept somewhere, finds it gone. Set and cleared by the app,
// on its executor.
struct app_link {
  app_core *core = nullptr;
};

// The view running now: the app it belongs to, none for a view written once (to_html), and the component whose view it
// is with the props of that view, none for the app's own.
struct view_context {
  std::shared_ptr<app_link> app{};
  held_view held{};
};

// What a callback holds of the view that made it: its component weakly, so that one its own component keeps leaves no
// cycle, and the props of that view, which hold only what placed the component.
struct call_target {
  std::shared_ptr<app_link> app{};
  std::weak_ptr<instance> owner{};
  std::shared_ptr<void const> props{};
  bool of_component = false;
};

// The context of the view running on this thread, empty outside one.
[[nodiscard]] view_context const &current_view() noexcept;

// What a callback made now holds.
[[nodiscard]] call_target current_target() noexcept;

// Makes `context` the current view's while it lives, then gives back the one before: views run one inside another.
class view_scope {
public:
  explicit view_scope(view_context context) noexcept;

  view_scope(view_scope const &) = delete;
  view_scope(view_scope &&) = delete;
  view_scope &operator=(view_scope const &) = delete;
  view_scope &operator=(view_scope &&) = delete;

  ~view_scope();

private:
  view_context previous_;
};

// Runs `work` as a task of the component `target` names, or of its app for none, as one of its handlers would: false if
// that component or that app is gone, or the app stopped, which runs nothing more.
[[nodiscard]] bool spawn_in(call_target const &target, async::task<> work);

// A task that is done at once.
[[nodiscard]] async::task<> done();

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_VIEW_CONTEXT_HPP
