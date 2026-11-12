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

#ifndef QUOLL_UI_APP_HPP
#define QUOLL_UI_APP_HPP

#include <quoll/async/any_executor.hpp>
#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/app_options.hpp>
#include <quoll/ui/component.hpp>
#include <quoll/ui/detail/app_core.hpp>
#include <quoll/ui/detail/bind_contexts.hpp>
#include <quoll/ui/detail/component_table.hpp>
#include <quoll/ui/detail/hot.hpp>
#include <quoll/ui/detail/link_states.hpp>
#include <quoll/ui/detail/run_effects.hpp>
#include <quoll/ui/element.hpp>
#include <quoll/ui/mounted_component.hpp>
#include <quoll/ui/node_id.hpp>
#include <quoll/ui/renderer.hpp>
#include <quoll/ui/renderer_ref.hpp>
#include <quoll/ui/state.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/**
 * @brief Shows a component through a renderer, and keeps it up to date.
 *
 * Whenever a state changes, the view runs again on the executor, once however many changed in between, and only what
 * differs reaches the renderer. Events come in through dispatch(); each runs as a task of its own, stopped and awaited
 * by stop().
 *
 * Neither copied nor moved: handlers and states point to the component it holds. Once started, await stop() before
 * destroying the app.
 *
 * Thread safety: none. Its functions and the tasks of its handlers run on one executor, one thing at a time.
 */
template <Component C, Renderer R>
class app {
  static_assert(std::is_aggregate_v<C>, "quoll::ui: a component is an aggregate: public members and no constructor");

public:
  template <async::Executor E>
  app(E executor, R renderer, app_options options = {})
      : renderer_(std::move(renderer)), core_(async::any_executor{executor}, renderer_ref{renderer_}, &view_of,
                                              detail::unmounted_of<C>(), detail::effects_of<C>(), detail::finds_of<C>(),
                                              detail::provides_of<C>(), &component_, std::move(options)) {
    if constexpr (detail::binds_of<C>() != nullptr) {
      detail::binds_of<C>()(&component_, core_.contexts());
    }

    detail::link_states(component_, {&core_, &detail::app_core::state_changed});

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

  /** @brief Shows the component for the first time, and starts its mounted(). Nothing, once stopped. */
  void start() {
    if (core_.stopped()) {
      return;
    }

    try {
      core_.render();

      if constexpr (MountedComponent<C>) {
        core_.mount(component_.mounted());
      }
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
   * are unmounted, their nodes left as they are. The
   * components of the view go first, each after those of its own view, then the component's unmounted() runs, if it has
   * one and its view showed. One at a time. Throws `std::logic_error` from one of the app's own tasks, which would wait
   * for itself.
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

  /** @brief The component. */
  [[nodiscard]] C &component() noexcept { return component_; }

  /** @brief The renderer. */
  [[nodiscard]] R &renderer() noexcept { return renderer_; }

private:
  static element view_of(void *component) { return static_cast<C *>(component)->view(); }

  [[nodiscard]] void *context_of(async::detail::type_id type) const {
    auto *const entry = core_.context(type);

    if (entry == nullptr) {
      throw std::logic_error{"quoll::ui::app: no context of this type was given"};
    }

    return entry->object();
  }

  // The core outlives the component, whose states call it, even from the destructor of a member.
  R renderer_;
  detail::app_core core_;
  C component_{};
};

} // namespace quoll::ui

#endif // QUOLL_UI_APP_HPP
