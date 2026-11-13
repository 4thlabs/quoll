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

#ifndef QUOLL_UI_APP_APP_HPP
#define QUOLL_UI_APP_APP_HPP

#include <quoll/async/any_executor.hpp>
#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/app/app_options.hpp>
#include <quoll/ui/component/component.hpp>
#include <quoll/ui/detail/app/app_core.hpp>
#include <quoll/ui/detail/view/hot.hpp>
#include <quoll/ui/view/element.hpp>
#include <quoll/ui/view/node_id.hpp>
#include <quoll/ui/view/renderer.hpp>
#include <quoll/ui/view/renderer_ref.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/**
 * @brief Shows a component, its root, through a renderer, and keeps it up to date.
 *
 * @code
 * inline constexpr component<> counter = [] {
 *   auto &count = use_state(0);
 *   auto const add = [&count] { count = *count + 1; };
 *
 *   return button(text(std::to_string(*count))) | on_click(add);
 * };
 *
 * app shown{executor, memory_renderer{}, counter};
 * shown.start();
 * @endcode
 *
 * Whenever a state changes, the view runs again on the executor, once however many changed in between, and only what
 * differs reaches the renderer. Events come in through dispatch(); each runs as a task of its own, stopped and awaited
 * by stop().
 *
 * Neither copied nor moved: handlers point to what it holds. Once started, await stop() before destroying the app.
 *
 * Thread safety: none. Its functions and the tasks of its handlers run on one executor, one thing at a time.
 */
template <Renderer R>
class app {
public:
  template <async::Executor E>
  app(E executor, R renderer, component<> root, app_options options = {})
      : renderer_(std::move(renderer)), root_(root),
        core_(async::any_executor{executor}, renderer_ref{renderer_}, &view_of, &root_, std::move(options)) {
#if defined(QUOLL_UI_HOT)
    detail::hot_subscribe(&core_, &detail::app_core::reloaded);
#endif

    core_.made();
  }

  app(app const &) = delete;
  app(app &&) = delete;
  app &operator=(app const &) = delete;
  app &operator=(app &&) = delete;

  ~app() {
    core_.going();

#if defined(QUOLL_UI_HOT)
    detail::hot_unsubscribe(&core_);
#endif

    core_.forget_view();
  }

  /** @brief Shows its root for the first time. Nothing, once stopped. */
  void start() {
    if (core_.stopped()) {
      return;
    }

    try {
      core_.render();
    } catch (...) {
      core_.abandon();
      throw;
    }
  }

  /**
   * @brief Tells on_error of a failure that comes from outside the app's tasks, as a renderer's that happens once its
   * call has returned: a widget's module failing to load in a browser.
   */
  void report_failure(std::exception_ptr failure) const noexcept { core_.report_failure(std::move(failure)); }

  /**
   * @brief Reports an event of a node: runs what the element showing it has for `name`. An event for a node that is
   * gone by now is ignored, as it may have been sent just before, and so is one coming after stop(): nothing would wait
   * for its task.
   */
  void dispatch(node_id id, std::string_view name, std::string value = {}) {
    core_.dispatch(id, name, std::move(value));
  }

  /**
   * @brief Asks the running handlers to stop, and completes once they have ended. Nothing shows afterwards: its widgets
   * are unmounted, their nodes left as they are, and its components go, each after those of its own view. Throws
   * `std::logic_error` from one of the app's own tasks, which would wait for itself.
   */
  [[nodiscard]] async::task<> stop() { return core_.stop(); }

  /**
   * @brief Its context of type T, as provide<T>() gave it.
   * @throws std::logic_error when it was given none.
   */
  template <class T>
    requires(!std::is_const_v<T>)
  [[nodiscard]] T &context() {
    return *static_cast<T *>(context_of(async::detail::type_id_of<T>()));
  }

  /** @brief See context(). */
  template <class T>
    requires(!std::is_const_v<T>)
  [[nodiscard]] T const &context() const {
    return *static_cast<T const *>(context_of(async::detail::type_id_of<T>()));
  }

  /** @brief The renderer. */
  [[nodiscard]] R &renderer() noexcept { return renderer_; }

private:
  static element view_of(void const *root) { return (*static_cast<component<> const *>(root))(); }

  [[nodiscard]] void *context_of(async::detail::type_id type) const {
    auto *const entry = core_.context(type);

    if (entry == nullptr) {
      throw std::logic_error{"quoll::ui::app: no context of this type was given"};
    }

    return entry->object();
  }

  R renderer_;
  component<> root_;
  // Its components, whose hooks call it, go with it.
  detail::app_core core_;
};

} // namespace quoll::ui

#endif // QUOLL_UI_APP_APP_HPP
