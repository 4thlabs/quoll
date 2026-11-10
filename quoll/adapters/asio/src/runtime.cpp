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

#include <quoll/async/detail/synchronized.hpp>

#include <quoll/asio/executor.hpp>
#include <quoll/async/any_executor.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detail/is_a_stop.hpp>
#include <quoll/async/detail/report.hpp>
#include <quoll/async/report.hpp>
#include <quoll/async/task.hpp>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/signal_set.hpp>
#include <asio/strand.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <signal.h>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

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
  // On a strand of its own: its handler and release() may otherwise run on two threads of the loop at once.
  stop_signals(::asio::strand<::asio::io_context::executor_type> const &strand, std::stop_source &stop)
      : signals_(strand), stop_(&stop) {
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
      // The former actions back first: a task the stop wakes, on another thread, then finds them.
      if (!error) {
        release();
        stop_->request_stop();
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

namespace detail {

// What makes the runtime's strands: the service its executors offer.
struct runtime_strands {
  runtime::state *state;

  [[nodiscard]] async::any_executor make_strand() const;
};

static_assert(async::Strands<runtime_strands>);

} // namespace detail

namespace {

// A strand of the runtime's loop, shared by the copies of an executor.
struct runtime_strand {
  asio::executor<::asio::strand<::asio::io_context::executor_type>> loop;
};

} // namespace

struct runtime::state {
  explicit state(async::runtime_options options)
      : threads(options.threads == 0 ? 1 : options.threads),
        context(static_cast<int>(std::min<std::size_t>(threads, std::numeric_limits<int>::max()))),
        loop(context.get_executor(), threads > 1 ? object_strands::own : object_strands::shared) {}

  std::size_t threads;
  // A hint of 1 tells asio that one thread runs it, which it optimizes for.
  ::asio::io_context context;
  // What the runtime's services refer to: as long-lived as the runtime. Tasks hold executors, not this. Its sockets,
  // acceptors and timers are each on a strand of their own on several threads.
  asio::executor<::asio::io_context::executor_type> loop;
  detail::runtime_strands strands{this};
  // Where the signals wait, and what touches them while the loop runs.
  ::asio::strand<::asio::io_context::executor_type> signals_strand{::asio::make_strand(context)};
  std::stop_source stop;
  std::optional<stop_signals> signals;
  async::detail::synchronized<std::weak_ptr<void>> work;

  // Whether no executor of the runtime is alive: its tasks have ended.
  [[nodiscard]] bool idle() const {
    return work.with([](std::weak_ptr<void> const &held) { return held.expired(); });
  }
};

namespace {

// Keeps the loop running while an executor of the runtime is alive, even while its task waits for work elsewhere (a
// thread pool).
class work_token {
public:
  explicit work_token(runtime::state &shared) : state_(&shared), guard_(shared.context.get_executor()) {}

  work_token(work_token const &) = delete;
  work_token(work_token &&) = delete;
  work_token &operator=(work_token const &) = delete;
  work_token &operator=(work_token &&) = delete;

  // The last executor is gone: the signals no longer have a task to stop, and would keep the loop running. Posted while
  // the guard still holds the loop, which then runs this before it ends, and before the runtime can go.
  // Under memory pressure, a post that throws std::bad_alloc here ends the program, as a destructor cannot report it.
  ~work_token() {
    auto const release = [shared = state_] {
      if (shared->idle() && shared->signals) {
        shared->signals->release();
      }
    };

    // On the strand of the signals' handler, which may run at the same time on another thread: the runtime's, which
    // does not change, as the signals themselves may be made again by get_stop_token().
    ::asio::post(state_->signals_strand, release);
  }

private:
  runtime::state *state_;
  ::asio::executor_work_guard<::asio::io_context::executor_type> guard_;
};

} // namespace

void runtime::executor::post(async::work_item &item) const {
  if (strand_) {
    static_cast<runtime_strand const *>(strand_.get())->loop.post(item);
  } else {
    state_->loop.post(item);
  }
}

async::strands_ref runtime::executor::query(async::service_tag<async::strands_ref>) const {
  return async::strands_ref{state_->strands};
}

io::network_ref runtime::executor::query(async::service_tag<io::network_ref>) const {
  return io::network_ref{state_->loop};
}

async::timer_ref runtime::executor::query(async::service_tag<async::timer_ref>) const {
  return async::timer_ref{state_->loop};
}

async::offloader_ref runtime::executor::query(async::service_tag<async::offloader_ref>) const {
  return async::offloader_ref{state_->loop};
}

#if defined(QUOLL_ASIO_TLS)
io::tls_network_ref runtime::executor::query(async::service_tag<io::tls_network_ref>) const {
  return io::tls_network_ref{state_->loop};
}
#endif

runtime::runtime() : runtime(async::runtime_options{}) {}

runtime::runtime(async::runtime_options options) : state_(std::make_unique<state>(options)) {}

runtime::~runtime() = default;

namespace {

// An executor of the runtime, on a strand of its own when it runs several threads.
[[nodiscard]] std::shared_ptr<void> work_of(runtime::state &shared) {
  return shared.work.with([&shared](std::weak_ptr<void> &held) {
    auto token = held.lock();

    if (!token) {
      token = std::make_shared<work_token>(shared);
      held = token;
    }

    return token;
  });
}

[[nodiscard]] std::shared_ptr<void> strand_of(runtime::state &shared) {
  if (shared.threads == 1) {
    return nullptr;
  }

  return std::make_shared<runtime_strand>(
      runtime_strand{asio::executor<::asio::strand<::asio::io_context::executor_type>>{
          ::asio::make_strand(shared.context.get_executor())}});
}

} // namespace

async::any_executor detail::runtime_strands::make_strand() const {
  return async::any_executor{runtime::executor{*state, work_of(*state), strand_of(*state)}};
}

runtime::executor runtime::get_executor() { return executor{*state_, work_of(*state_), strand_of(*state_)}; }

std::stop_token runtime::get_stop_token() {
  if (state_->stop.stop_requested()) {
    state_->stop = std::stop_source{};
  }

  if (!state_->signals || state_->signals->released()) {
    state_->signals.reset();
    state_->signals.emplace(state_->signals_strand, state_->stop);
  }

  return state_->stop.get_token();
}

void runtime::run() {
  // Signals with no task to stop would keep the loop running.
  if (state_->idle() && state_->signals) {
    state_->signals->release();
  }

  state_->context.restart();

  // A handler's exception stops the loop on every thread, and the first one is thrown once they are joined: waiting
  // for the work to drain instead could wait for ever, as the tasks still suspended hold it.
  async::detail::synchronized<std::exception_ptr> failure;
  auto const run_loop = [this, &failure] {
    try {
      state_->context.run();
    } catch (...) {
      failure.with([](std::exception_ptr &first) {
        if (!first) {
          first = std::current_exception();
        }
      });
      state_->context.stop();
    }
  };

  // The others run the loop beside this thread, and are joined before it returns. One the system refuses leaves the
  // work to those started and to this one: the tasks still end, and the failure goes on after.
  std::vector<std::jthread> others;
  std::exception_ptr refused;

  try {
    others.reserve(state_->threads - 1);

    for (std::size_t index = 1; index < state_->threads; ++index) {
      others.emplace_back(run_loop);
    }
  } catch (...) {
    refused = std::current_exception();
  }

  run_loop();
  others.clear();

  if (auto const first = failure.with([](std::exception_ptr const &stored) { return stored; }); first) {
    std::rethrow_exception(first);
  }

  if (refused) {
    std::rethrow_exception(refused);
  }
}

int runtime::run(async::task<> main) {
  // Shared with the handler, which runs after run() if run() throws while the task is still under way.
  auto const failed = std::make_shared<std::atomic<bool>>(false);

  // Once the program's task ends, the others spawned with the runtime's stop token are asked to stop, as a Ctrl-C
  // would, so that the loop drains rather than run them for ever, as tokio drops its tasks once main has returned.
  struct outcome {
    std::shared_ptr<std::atomic<bool>> failed;
    // Mutable: requesting a stop changes its shared state, not the handler.
    mutable std::stop_source stop;

    void operator()() const noexcept { stop.request_stop(); }

    // A stop it was asked for, as a Ctrl-C, is no failure; one nothing asked for is.
    void operator()(std::exception_ptr const &failure) const noexcept {
      if (!async::detail::is_a_stop(failure) || !stop.stop_requested()) {
        async::detail::report_always("the program's main task ended on an exception", failure);
        failed->store(true);
      }

      stop.request_stop();
    }
  };

  auto const stop_token = get_stop_token();

  async::co_spawn(get_executor(), std::move(main), outcome{failed, state_->stop}, stop_token);
  run();
  return failed->load() ? EXIT_FAILURE : EXIT_SUCCESS;
}

::asio::io_context &runtime::context() noexcept { return state_->context; }

} // namespace quoll::asio
