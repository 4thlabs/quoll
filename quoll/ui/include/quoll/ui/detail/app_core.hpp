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

#ifndef QUOLL_UI_DETAIL_APP_CORE_HPP
#define QUOLL_UI_DETAIL_APP_CORE_HPP

#include <quoll/async/any_executor.hpp>
#include <quoll/async/detail/task_group.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/app_options.hpp>
#include <quoll/ui/detail/held_view.hpp>
#include <quoll/ui/detail/instance.hpp>
#include <quoll/ui/detail/view_context.hpp>
#include <quoll/ui/element.hpp>
#include <quoll/ui/node_id.hpp>
#include <quoll/ui/renderer_ref.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::ui::detail {

// What an app does, whatever its component and its renderer: nothing here is a template, so it is compiled once.
//
// Thread safety: none. Its functions and the tasks it runs are on one executor, one thing at a time.
class app_core {
public:
  using view_function = element (*)(void *component);
  using unmounted_function = void (*)(void *component);

  // `unmounted`, none for a component without it, runs as the app stops, once its view showed.
  app_core(async::any_executor executor, renderer_ref renderer, view_function view, unmounted_function unmounted,
           void *component, app_options options);

  app_core(app_core const &) = delete;
  app_core(app_core &&) = delete;
  app_core &operator=(app_core const &) = delete;
  app_core &operator=(app_core &&) = delete;

  // Callbacks kept past it find it gone.
  ~app_core();

  [[nodiscard]] bool stopped() const noexcept { return stopped_; }

  void render();
  // Under the stop of the component `held` names, kept with its props until it ends, or the app's for none.
  void spawn(async::task<> work, held_view held = {});
  // spawn() for a mounted(), which does not start once its component, or the app, is stopped.
  void mount(async::task<> work, held_view held = {});
  void dispatch(node_id id, std::string_view name, std::string value);

  // As a handler's failure: to on_error.
  void report_failure(std::exception_ptr failure) const noexcept { failed(std::move(failure)); }

  [[nodiscard]] async::task<> stop();

  // A start that failed: what the view left pending, or the mounted() it spawned, must show nothing more. stop() still
  // waits for them.
  void abandon() noexcept;

  // What the view showed, and so the handlers it holds, which may refer to the component: gone before the component.
  void forget_view() noexcept;

  static void state_changed(void *self);

  // A hot reload changed the literals of the views: none is given again from before it, as no state says so.
  static void reloaded(void *self);

private:
  struct outcome;

  [[nodiscard]] async::task<> render_later();
  // As the app stops: what a renderer throws goes to on_error, and the others are unmounted all the same.
  void unmount_widgets(std::vector<element> const &shown) const noexcept;
  void failed(std::exception_ptr failure) const noexcept;
  static void failed_in(void *self, std::exception_ptr failure) noexcept;

  async::any_executor executor_;
  renderer_ref renderer_;
  // What callbacks hold of it, to start their work here, as long as it lives.
  std::shared_ptr<app_link> link_;
  view_function view_;
  unmounted_function unmounted_;
  void *component_;
  app_options options_;
  std::stop_source stop_;
  async::detail::task_group tasks_;
  std::vector<element> shown_;
  // The components of the view, kept from one render to the next.
  std::vector<std::shared_ptr<instance>> components_;
  // Every component made, for as long as something holds it: stop() refuses to be awaited from their tasks too.
  std::vector<std::weak_ptr<instance>> instances_;
  node_id last_id_ = no_node;
  bool render_pending_ = false;
  // Counts the hot reloads: a component's last view, from an epoch before, is not given again.
  std::uint64_t epoch_ = 0;
  bool stopped_ = false;
  // Its view showed, and its unmounted() has not run yet.
  bool shown_once_ = false;
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_APP_CORE_HPP
