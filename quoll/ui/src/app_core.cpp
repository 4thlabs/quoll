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
#include <quoll/ui/detail/reconciler.hpp>

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

  void operator()() const noexcept { self->tasks_.ended(); }

  // A stop is no failure: on_error does not hear of it.
  void operator()(async::stopped_t) const noexcept { self->tasks_.ended(); }

  void operator()(std::exception_ptr failure) const noexcept {
    self->failed(std::move(failure));
    self->tasks_.ended();
  }
};

app_core::app_core(async::any_executor executor, renderer_ref renderer, view_function view, void *component,
                   app_options options)
    : executor_(std::move(executor)), renderer_(renderer), view_(view), component_(component),
      options_(std::move(options)) {}

void app_core::render() {
  std::vector<element> next;

  append_child(next, view_(component_));
  reconciler{renderer_, last_id_}.children(no_node, shown_, next);
  shown_ = std::move(next);
}

void app_core::dispatch(node_id id, std::string_view name, std::string value) {
  auto const *const target = stopped_ ? nullptr : find_shown(shown_, id);

  if (target == nullptr) {
    return;
  }

  for (auto const &binding : target->events) {
    if (binding.name == name) {
      spawn(run_handler(binding.handler, value));
    }
  }
}

async::task<> app_core::stop() {
  if (co_await async::current_stop_token() == stop_.get_token()) {
    throw std::logic_error{"quoll::ui::app: stop() awaited from one of its own tasks"};
  }

  abandon();
  co_await tasks_.join();
}

void app_core::abandon() noexcept {
  stopped_ = true;
  stop_.request_stop();
}

void app_core::forget_view() noexcept { shown_.clear(); }

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

void app_core::spawn(async::task<> work) {
  tasks_.started();

  try {
    async::co_spawn(executor_, std::move(work), outcome{this}, stop_.get_token());
  } catch (...) {
    tasks_.ended();
    throw;
  }
}

} // namespace quoll::ui::detail
