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

#include <quoll/ui/detail/app_core.hpp>

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/stopped.hpp>
#include <quoll/ui/detail/expand.hpp>
#include <quoll/ui/detail/reconciler.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace quoll::ui::detail {

namespace {

// Holds a copy of the handler until its task ends: the element it came from may be gone by then. Calling it here sends
// what it throws to on_error. stop() may have come before this starts, and nothing starts after it.
[[nodiscard]] async::task<> run_handler(event_handler handler, std::string value) {
  auto const stop = co_await async::current_stop_token();

  if (stop.stop_requested()) {
    co_return;
  }

  co_await handler(std::move(value));
}

// What a component does once it first shows, unless it went before this starts: a stop asked for since it was posted,
// with its unmounted() run already.
[[nodiscard]] async::task<> run_mounted(async::task<> work) {
  auto const stop = co_await async::current_stop_token();

  if (stop.stop_requested()) {
    co_return;
  }

  co_await std::move(work);
}

[[nodiscard]] element const *find_shown(std::vector<element> const &elements, node_id id) noexcept {
  for (auto const &value : elements) {
    if (value.id == id) {
      return &value;
    }

    if (auto const *const nested = find_shown(value.children, id)) {
      return nested;
    }
  }

  return nullptr;
}

} // namespace

struct app_core::outcome {
  app_core *self;
  // Kept until its task ends: the component it runs on goes then, once unmounted, and before the task counts as ended,
  // which stop() waits for, as the app may go right after. Its props hold what they may refer to (see held_view): the
  // order is free.
  mutable held_view held;

  void ended() const noexcept {
    held = {};
    self->tasks_.ended();
  }

  void operator()() const noexcept { ended(); }

  // A stop is no failure: on_error does not hear of it.
  void operator()(async::stopped_t) const noexcept { ended(); }

  void operator()(std::exception_ptr failure) const noexcept {
    self->failed(std::move(failure));
    ended();
  }
};

app_core::app_core(async::any_executor executor, renderer_ref renderer, view_function view,
                   unmounted_function unmounted, void *component, app_options options)
    : executor_(std::move(executor)), renderer_(renderer), link_(std::make_shared<app_link>(app_link{this})),
      view_(view), unmounted_(unmounted), component_(component), options_(std::move(options)) {}

app_core::~app_core() { link_->core = nullptr; }

void app_core::render() {
  std::vector<element> next;
  auto view = [this] {
    // Its callbacks call the app's own component back.
    view_scope const scope{{.app = link_}};

    return view_(component_);
  }();
  expansion made{.app = link_,
                 .app_stop = stop_.get_token(),
                 .link = {this, &app_core::state_changed},
                 .sink = {this, &app_core::failed_in}};

  if (!components_.empty() || has_component(view)) {
    expand(view, components_, made);
  }

  try {
    // For stop() to know a task of theirs, as long as one lives.
    std::erase_if(instances_, [](auto const &held) { return held.expired(); });

    for (auto const &component : made.made) {
      instances_.push_back(component);
    }

    append_child(next, std::move(view));
    reconciler{renderer_, last_id_}.children(no_node, shown_, next);
  } catch (...) {
    discard(made);
    throw;
  }

  // Shown: the components follow, and nothing throws until they have.
  commit(made, components_);
  shown_ = std::move(next);
  shown_once_ = true;

  // Once shown, as the app's own. One that cannot start is told as a failure, and the others start all the same.
  for (auto const &component : made.made) {
    if (component->table().mounted == nullptr) {
      continue;
    }

    try {
      mount(component->table().mounted(component->component()), {.owner = component, .props = component->props()});
    } catch (...) {
      failed(std::current_exception());
    }
  }
}

void app_core::dispatch(node_id id, std::string_view name, std::string value) {
  auto const *const target = stopped_ ? nullptr : find_shown(shown_, id);

  if (target == nullptr) {
    return;
  }

  for (auto const &binding : target->events) {
    if (binding.name == name) {
      spawn(run_handler(binding.handler, value), binding.held);
    }
  }
}

async::task<> app_core::stop() {
  auto const awaiter = co_await async::current_stop_token();
  auto const own = awaiter == stop_.get_token() || std::ranges::any_of(instances_, [&awaiter](auto const &held) {
                     auto const component = held.lock();

                     return component && component->stop_token() == awaiter;
                   });

  if (own) {
    throw std::logic_error{"quoll::ui::app: stop() awaited from one of its own tasks"};
  }

  abandon();
  co_await tasks_.join();
}

void app_core::abandon() noexcept {
  stopped_ = true;
  stop_.request_stop();

  // The components go as the app stops, their unmounted() with them; what one throws goes where a handler's does.
  for (auto const &component : components_) {
    component->unmount({this, &app_core::failed_in});
  }

  components_.clear();

  // Last, as a component goes after those of its view.
  if (!shown_once_ || unmounted_ == nullptr) {
    return;
  }

  shown_once_ = false;

  try {
    unmounted_(component_);
  } catch (...) {
    failed(std::current_exception());
  }
}

void app_core::forget_view() noexcept {
  shown_.clear();

  // Empty once the app stopped, as it must before it goes: what one destroyed without its stop has left holds no cycle.
  for (auto const &component : components_) {
    component->detach();
  }

  components_.clear();
}

void app_core::failed_in(void *self, std::exception_ptr failure) noexcept {
  static_cast<app_core const *>(self)->failed(std::move(failure));
}

async::task<> app_core::render_later() {
  render_pending_ = false;

  if (!stopped_) {
    render();
  }

  co_return;
}

void app_core::state_changed(void *self) {
  auto &owner = *static_cast<app_core *>(self);

  if (owner.render_pending_ || owner.stopped_) {
    return;
  }

  owner.render_pending_ = true;

  try {
    owner.spawn(owner.render_later());
  } catch (...) {
    // Not spawned: the next change has to try again.
    owner.render_pending_ = false;
    throw;
  }
}

void app_core::failed(std::exception_ptr failure) const noexcept {
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

void app_core::mount(async::task<> work, held_view held) { spawn(run_mounted(std::move(work)), std::move(held)); }

void app_core::spawn(async::task<> work, held_view held) {
  auto const stop_token = held.owner ? held.owner->stop_token() : stop_.get_token();

  tasks_.started();

  try {
    async::co_spawn(executor_, std::move(work), outcome{.self = this, .held = std::move(held)}, stop_token);
  } catch (...) {
    tasks_.ended();
    throw;
  }
}

} // namespace quoll::ui::detail
