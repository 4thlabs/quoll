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

#ifndef QUOLL_UI_DETAIL_VIEW_VIEW_CONTEXT_HPP
#define QUOLL_UI_DETAIL_VIEW_VIEW_CONTEXT_HPP

#include <quoll/async/task.hpp>
#include <quoll/ui/detail/view/held_view.hpp>

#include <memory>

namespace quoll::ui::detail {

class app_core;

// An app as long as it lives: what outlives it, a callback kept somewhere, finds it gone. Set and cleared by the app,
// on its executor.
struct app_link {
  app_core *core = nullptr;
};

// The view running now: the app it belongs to, none for a view written once (to_html), and the component whose view it
// is with the props of that view, none for the app's own. Thread safety: a value, the current one per thread.
struct view_context {
  std::shared_ptr<app_link> app{};
  held_view held{};
};

// What a callback holds of the view that made it, weakly: its component, and the props of that view, which hold what
// placed the component, so that a callback kept by its component, or by any of those above, leaves no cycle and keeps
// nothing of them once they go. Thread safety: a value.
struct call_target {
  std::shared_ptr<app_link> app{};
  std::weak_ptr<instance> owner{};
  std::weak_ptr<void const> props{};
  bool of_component = false;
  // It had props: once they are gone, the view that made it has viewed again, and what it refers to may be gone.
  bool of_props = false;
};

// What a callback made now holds, of the view running on this thread: nothing outside one.
[[nodiscard]] call_target current_target() noexcept;

// Makes `context` the current view's while it lives, then gives back the one before: views run one inside another.
// Thread safety: on the thread of the view it sets, as the view it wraps.
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
// that component or that app is gone, or the app stopped, which runs nothing more. Throws std::logic_error once the
// view that made the callback has been replaced, its props gone.
[[nodiscard]] bool spawn_in(call_target const &target, async::task<> work);

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_VIEW_VIEW_CONTEXT_HPP
