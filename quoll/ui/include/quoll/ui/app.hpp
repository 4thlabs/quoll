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
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detail/task_group.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/component.hpp>
#include <quoll/ui/detail/reconciler.hpp>
#include <quoll/ui/element.hpp>
#include <quoll/ui/node_id.hpp>
#include <quoll/ui/renderer.hpp>
#include <quoll/ui/state.hpp>

#include <glaze/reflection/to_tuple.hpp>

#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::ui {

/** @brief What an app does with what its handlers throw. */
struct app_options {
  /**
   * @brief Called with what a handler threw, other than `operation_canceled`, on the executor; must not throw. Empty:
   * its message goes to the standard error, which is the console in a browser.
   */
  std::function<void(std::exception_ptr)> on_error{};
};

namespace detail {

template <class T>
inline constexpr bool is_state = false;

template <class T>
inline constexpr bool is_state<state<T>> = true;

// Only the members, not their names: a component may have internal linkage, and this part of Glaze compiles much faster
// than what also reflects names, which every rebuild of an interface pays for.
template <class C>
void link_states(C &component, state_link link) {
  auto members = glz::to_tie(component);

  [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    [[maybe_unused]] auto const link_one = [&link](auto &member) {
      if constexpr (is_state<std::remove_cvref_t<decltype(member)>>) {
        member.link(link);
      }
    };

    (link_one(glz::get<Index>(members)), ...);
  }(std::make_index_sequence<glz::tuple_size_v<decltype(members)>>{});
}

} // namespace detail

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
      : executor_(std::move(executor)), renderer_(std::move(renderer)), options_(std::move(options)) {
    detail::link_states(component_, {this, &app::state_changed});
  }

  app(app const &) = delete;
  app(app &&) = delete;
  app &operator=(app const &) = delete;
  app &operator=(app &&) = delete;

  ~app() = default;

  /** @brief Shows the component for the first time, and starts its mounted(). Nothing, once stopped. */
  void start() {
    if (stopped_) {
      return;
    }

    render();

    if constexpr (MountedComponent<C>) {
      spawn(component_.mounted());
    }
  }

  /**
   * @brief Reports an event of a node: runs what the element showing it has for `name`. An event for a node that is
   * gone by now is ignored, as it may have been sent just before, and so is one coming after stop(): nothing would wait
   * for its task.
   */
  void dispatch(node_id id, std::string_view name, std::string value = {}) {
    auto const *const target = stopped_ ? nullptr : find(shown_, id);

    if (target == nullptr) {
      return;
    }

    for (auto const &binding : target->events) {
      if (binding.name == name) {
        spawn(run_handler(binding.handler, value));
      }
    }
  }

  /** @brief Asks the running handlers to stop, and completes once they have ended. Nothing shows afterwards. */
  [[nodiscard]] async::task<> stop() {
    stopped_ = true;
    stop_.request_stop();
    co_await tasks_.join();
  }

  /** @brief The component. */
  [[nodiscard]] C &component() noexcept { return component_; }

  /** @brief The renderer. */
  [[nodiscard]] R &renderer() noexcept { return renderer_; }

private:
  // Holds a copy of the handler until its task ends: the element it came from may be gone by then.
  [[nodiscard]] static async::task<> run_handler(event_handler handler, std::string value) {
    co_await handler(std::move(value));
  }

  [[nodiscard]] async::task<> render_later() {
    render_pending_ = false;

    if (!stopped_) {
      render();
    }

    co_return;
  }

  static void state_changed(void *self) {
    auto &owner = *static_cast<app *>(self);

    if (owner.render_pending_ || owner.stopped_) {
      return;
    }

    owner.render_pending_ = true;
    owner.spawn(owner.render_later());
  }

  void render() {
    std::vector<element> next;

    detail::append_child(next, component_.view());
    detail::reconciler<R>{renderer_, last_id_}.children(no_node, shown_, next);
    shown_ = std::move(next);
  }

  [[nodiscard]] static element const *find(std::vector<element> const &elements, node_id id) noexcept {
    for (auto const &value : elements) {
      if (value.id == id) {
        return &value;
      }

      if (auto const *const nested = find(value.children, id)) {
        return nested;
      }
    }

    return nullptr;
  }

  struct outcome {
    app *self;

    void operator()() const noexcept { self->tasks_.ended(); }

    void operator()(std::exception_ptr failure) const noexcept {
      self->failed(std::move(failure));
      self->tasks_.ended();
    }
  };

  void failed(std::exception_ptr failure) const noexcept {
    try {
      std::rethrow_exception(failure);
    } catch (std::system_error const &error) {
      if (error.code() == std::errc::operation_canceled) {
        return;
      }
    } catch (...) { // NOLINT(bugprone-empty-catch): reported below, whatever it is.
    }

    if (options_.on_error) {
      options_.on_error(std::move(failure));
      return;
    }

    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &error) {
      std::fprintf(stderr, "quoll::ui: a handler failed: %s\n", error.what());
    } catch (...) {
      std::fputs("quoll::ui: a handler failed\n", stderr);
    }
  }

  void spawn(async::task<> work) {
    tasks_.started();

    try {
      async::co_spawn(executor_, std::move(work), outcome{this}, stop_.get_token());
    } catch (...) {
      tasks_.ended();
      throw;
    }
  }

  async::any_executor executor_;
  R renderer_;
  app_options options_;
  std::stop_source stop_;
  async::detail::task_group tasks_;
  C component_{};
  std::vector<element> shown_;
  node_id last_id_ = no_node;
  bool render_pending_ = false;
  bool stopped_ = false;
};

} // namespace quoll::ui

#endif // QUOLL_UI_APP_HPP
