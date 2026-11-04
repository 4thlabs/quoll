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

#include <quoll/asio/runtime.hpp>

#include <quoll/asio/executor.hpp>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/signal_set.hpp>

#include <array>
#include <csignal>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <signal.h>
#include <stop_token>
#include <system_error>

namespace quoll::asio {

namespace {

// The action of a signal, saved and given back. POSIX keeps the whole sigaction (flags, mask); Windows has signal()
// only, which can read the handler only by setting another for an instant.
#if defined(_WIN32)
using signal_action = void (*)(int);

[[nodiscard]] inline signal_action current_action(int number) noexcept {
  auto const previous = std::signal(number, SIG_DFL);

  std::signal(number, previous);
  return previous;
}

inline void set_action(int number, signal_action const &action) noexcept { std::signal(number, action); }

[[nodiscard]] inline bool ignores(signal_action const &action) noexcept { return action == SIG_IGN; }

[[nodiscard]] inline bool is_default(signal_action const &action) noexcept { return action == SIG_DFL; }
#else
using signal_action = struct sigaction;

[[nodiscard]] inline signal_action current_action(int number) noexcept {
  signal_action action{};

  ::sigaction(number, nullptr, &action);
  return action;
}

inline void set_action(int number, signal_action const &action) noexcept { ::sigaction(number, &action, nullptr); }

[[nodiscard]] inline bool ignores(signal_action const &action) noexcept { return action.sa_handler == SIG_IGN; }

[[nodiscard]] inline bool is_default(signal_action const &action) noexcept { return action.sa_handler == SIG_DFL; }
#endif

// SIGINT and SIGTERM turned into a stop request while a runtime runs, unless the process ignores them (nohup, a
// background job). Their actions from before are saved, and given back by release(): at the first signal, so that the
// next one has its usual effect (ending a process whose tasks do not stop), and once the tasks are done.
class stop_signals {
public:
  stop_signals(::asio::io_context &context, std::stop_source &stop) : signals_(context), stop_(&stop) {
    try {
      for (std::size_t index = 0; index < numbers.size(); ++index) {
        saved_[index] = current_action(numbers[index]);

        if (!ignores(saved_[index])) {
          signals_.add(numbers[index]);
          taken_[index] = true;
        }
      }
    } catch (...) {
      // Gives back what was taken: the destructor does not run for an object that was not built.
      release();
      throw;
    }

    signals_.async_wait([this](std::error_code error, int) {
      if (!error) {
        stop_->request_stop();
        release();
      }
    });
  }

  stop_signals(stop_signals const &) = delete;
  stop_signals(stop_signals &&) = delete;
  stop_signals &operator=(stop_signals const &) = delete;
  stop_signals &operator=(stop_signals &&) = delete;

  ~stop_signals() { release(); }

  [[nodiscard]] bool released() const noexcept { return released_; }

  void release() noexcept {
    if (released_) {
      return;
    }

    std::error_code ignored;

    // Ends the wait, which would keep the loop running; then asio sets the default action as it lets a signal go that
    // no other signal_set of the program waits for, and the saved one replaces it. A signal another set still waits
    // for keeps asio's handler.
    signals_.cancel(ignored);
    signals_.clear(ignored);

    for (std::size_t index = 0; index < numbers.size(); ++index) {
      if (taken_[index] && is_default(current_action(numbers[index]))) {
        set_action(numbers[index], saved_[index]);
      }
    }

    released_ = true;
  }

private:
  static constexpr std::array<int, 2> numbers{SIGINT, SIGTERM};

  ::asio::signal_set signals_;
  std::stop_source *stop_;
  std::array<signal_action, 2> saved_{};
  std::array<bool, 2> taken_{};
  bool released_ = false;
};

} // namespace

struct runtime::state {
  ::asio::io_context context{1};
  // What the runtime's services refer to: as long-lived as the runtime. Tasks hold executors, not this.
  asio::executor<::asio::io_context::executor_type> loop{context.get_executor()};
  std::stop_source stop;
  std::optional<stop_signals> signals;
  std::mutex work_mutex;
  std::weak_ptr<void> work;

  // Whether no executor of the runtime is alive: its tasks have ended.
  [[nodiscard]] bool idle() {
    std::lock_guard const lock{work_mutex};

    return work.expired();
  }
};

namespace {

// Keeps the loop running while an executor of the runtime is alive, even while its task waits for work elsewhere (a
// thread pool).
class work_token {
public:
  work_token(runtime::state &shared) : state_(&shared), guard_(shared.context.get_executor()) {}

  work_token(work_token const &) = delete;
  work_token(work_token &&) = delete;
  work_token &operator=(work_token const &) = delete;
  work_token &operator=(work_token &&) = delete;

  // The last executor is gone: the signals no longer have a task to stop, and would keep the loop running. Posted while
  // the guard still holds the loop, which then runs this before it ends, and before the runtime can go.
  ~work_token() {
    ::asio::post(state_->context, [shared = state_] {
      if (shared->idle() && shared->signals) {
        shared->signals->release();
      }
    });
  }

private:
  runtime::state *state_;
  ::asio::executor_work_guard<::asio::io_context::executor_type> guard_;
};

} // namespace

void runtime::executor::post(async::work_item &item) const { state_->loop.post(item); }

io::network_ref runtime::executor::query(async::service_tag<io::network_ref>) const {
  return io::network_ref{state_->loop};
}

async::timer_ref runtime::executor::query(async::service_tag<async::timer_ref>) const {
  return async::timer_ref{state_->loop};
}

#if defined(QUOLL_ASIO_TLS)
io::tls_network_ref runtime::executor::query(async::service_tag<io::tls_network_ref>) const {
  return io::tls_network_ref{state_->loop};
}
#endif

runtime::runtime() : state_(std::make_unique<state>()) {}

runtime::~runtime() = default;

runtime::executor runtime::get_executor() {
  std::lock_guard const lock{state_->work_mutex};
  auto work = state_->work.lock();

  if (!work) {
    work = std::make_shared<work_token>(*state_);
    state_->work = work;
  }

  return executor{*state_, std::move(work)};
}

std::stop_token runtime::get_stop_token() {
  if (state_->stop.stop_requested()) {
    state_->stop = std::stop_source{};
  }

  if (!state_->signals || state_->signals->released()) {
    state_->signals.reset();
    state_->signals.emplace(state_->context, state_->stop);
  }

  return state_->stop.get_token();
}

void runtime::run() {
  // Signals with no task to stop would keep the loop running.
  if (state_->idle() && state_->signals) {
    state_->signals->release();
  }

  state_->context.restart();
  state_->context.run();
}

::asio::io_context &runtime::context() noexcept { return state_->context; }

} // namespace quoll::asio
