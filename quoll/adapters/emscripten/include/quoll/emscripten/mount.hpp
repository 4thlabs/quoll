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

#ifndef QUOLL_EMSCRIPTEN_MOUNT_HPP
#define QUOLL_EMSCRIPTEN_MOUNT_HPP

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/task.hpp>
#include <quoll/emscripten/dom_renderer.hpp>
#include <quoll/emscripten/executor.hpp>
#include <quoll/ui/app/app.hpp>
#include <quoll/ui/component/component.hpp>
#include <quoll/ui/detail/view/hot.hpp>
#include <quoll/ui/view/node_id.hpp>

#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace quoll::emscripten {

namespace detail {

// What is left of a start that failed: nothing shows any more, and its tasks need it until they end.
template <class App>
[[nodiscard]] async::task<> discard(std::shared_ptr<App> failed) {
  co_await failed->stop();
}

// Whether the component of `root` is the root of an app mounted already.
[[nodiscard]] bool is_mounted(ui::detail::component_table const &root) noexcept;

// Room for one more app, made before it starts, so that keep_mounted() cannot throw.
void reserve_mounted();

// Keeps `app`, the app of `root`, for as long as the page: held without its type, its deleter made where the app is,
// in the program's sources, which a QUOLL_ switch may change.
void keep_mounted(ui::detail::component_table const &root, std::shared_ptr<void> app) noexcept;

} // namespace detail

/**
 * @brief Shows a component, the root of an app, in the page, in the element found by `selector`, for as long as the
 * page lives: what the main() of an interface calls.
 *
 * @code
 * int main() { quoll::emscripten::mount(notes_page, "#app"); }
 * @endcode
 *
 * Returning from main() leaves the event loop running, and the app with it.
 *
 * @throws std::invalid_argument when nothing matches `selector`.
 * @throws std::logic_error when this component is already mounted: one app for each.
 * @throws what the first view throws: the page is left as it was, and a later mount can try again.
 */
inline ui::app<dom_renderer> &
mount(ui::component_base<void> const &root, std::string_view selector = "#app", ui::app_options options = {}) {
  using app_type = ui::app<dom_renderer>;

  if (detail::is_mounted(root.table())) {
    throw std::logic_error{"quoll::emscripten::mount: this component is already mounted"};
  }

  // Its room first: once started, the app goes nowhere but into it, as one started must not go without its stop.
  detail::reserve_mounted();

  auto instance = std::make_shared<app_type>(executor{}, dom_renderer{selector}, root, std::move(options));

  auto const dispatch = [](void *owner, ui::node_id id, char const *name, char const *value) {
    static_cast<app_type *>(owner)->dispatch(id, name, value);
  };

  instance->renderer().on_event(dispatch, instance.get());

  // A widget's module failing to load or to mount, once its render has gone on: the app's on_error hears of it.
  auto const failed = [](void *owner, char const *message) {
    static_cast<app_type *>(owner)->report_failure(std::make_exception_ptr(std::runtime_error{message}));
  };

  instance->renderer().on_failure(failed, instance.get());

#if defined(QUOLL_UI_HOT)
  detail::quoll_emscripten_ui_hot_install([](char const *file, unsigned line, unsigned column, char const *value) {
    ui::detail::hot_replace(file, line, column, value);
  });
#endif

  try {
    instance->start();
  } catch (...) {
    instance->renderer().close();
    async::co_spawn(executor{}, detail::discard(std::move(instance)), async::detached);
    throw;
  }

  auto &shown = *instance;

  detail::keep_mounted(root.table(), std::move(instance));
  return shown;
}

} // namespace quoll::emscripten

#endif // QUOLL_EMSCRIPTEN_MOUNT_HPP
