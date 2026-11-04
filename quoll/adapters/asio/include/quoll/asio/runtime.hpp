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

#ifndef QUOLL_ASIO_RUNTIME_HPP
#define QUOLL_ASIO_RUNTIME_HPP

#include <quoll/asio/detail/stop_signals.hpp>
#include <quoll/asio/executor.hpp>
#include <quoll/async/offers_service.hpp>
#include <quoll/async/service.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/io/network_ref.hpp>
#include <quoll/io/tls_network_ref.hpp>

#include <asio/execution/outstanding_work.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/prefer.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace quoll::asio {

namespace detail {

// An executor of the io_context that counts as work while it exists: the loop runs as long as a task holds one, even
// while that task waits for work elsewhere (a thread pool).
using tracked_io_executor = std::decay_t<decltype(::asio::prefer(std::declval<::asio::io_context::executor_type>(),
                                                                 ::asio::execution::outstanding_work.tracked))>;

struct runtime_state {
  ::asio::io_context context{1};
  std::stop_source stop;
  std::optional<stop_signals> signals;
  // The runtime's executors alive: when none is left, its tasks have ended.
  std::atomic<std::size_t> executors{0};

  // The last executor is gone: the signals no longer have a task to stop, and would keep the loop running.
  void idle() {
    ::asio::post(context, [this] {
      if (executors.load() == 0 && signals) {
        signals->release();
      }
    });
  }
};

} // namespace detail

/**
 * @brief The asio runtime: an I/O loop run by the thread calling run(), on which a program's tasks run.
 *
 * @code
 * quoll::async::task<> serve_notes(std::uint16_t port) {
 *   quoll::http::router router;
 *
 *   quoll::rest::mount<notes::api>(router, notes_service{});
 *   co_await quoll::server::listen(router, port);
 * }
 *
 * int main() {
 *   quoll::asio::runtime runtime;
 *
 *   co_spawn(runtime.get_executor(), serve_notes(8080), quoll::async::detached, runtime.get_stop_token());
 *   runtime.run();
 * }
 * @endcode
 *
 * Its executor offers the loop's network (io::network_ref) and timer (async::timer_ref), so that io::connect(),
 * io::listen(), async::sleep_for() and what is built on them (quoll::server::listen, quoll::client::transport, a REST
 * client) need nothing handed to them.
 * What does I/O on the loop (a server, a client and its kept connections) lives in the tasks, created and destroyed by
 * them: the program's main coroutine rather than main().
 *
 * run() returns once no executor of the runtime is left (the tasks spawned on them have ended, the copies they made
 * are gone) and the work they started has drained: like an asio work guard, an executor kept in main() keeps run()
 * running, so hand get_executor() to co_spawn() directly. get_stop_token() turns the first SIGINT or SIGTERM (Ctrl-C)
 * into a stop request on that token, then gives the signals their former actions back, so that the next one ends a
 * process whose tasks do not stop; signals the process ignores stay ignored. The signals are the process's: one runtime
 * at a time takes them.
 *
 * Neither copied nor moved: its executors point to it, and it outlives them. Thread safety: run() on one thread;
 * the executors may be used from any thread, as asio's.
 */
class runtime {
public:
  /** @brief The executor of the runtime: posts to its loop, and offers its network, its timer and, with OpenSSL, TLS.
   */
  class executor {
  public:
    using services = detail::loop_services;

    executor(executor const &other) noexcept : inner_(other.inner_), state_(other.state_) { ++state_->executors; }

    // A moved-from executor counts no more; it can only be assigned to or destroyed.
    executor(executor &&other) noexcept
        : inner_(std::move(other.inner_)), state_(std::exchange(other.state_, nullptr)) {}

    executor &operator=(executor const &other) noexcept {
      if (this != &other) {
        ++other.state_->executors;
        release();
        inner_ = other.inner_;
        state_ = other.state_;
      }

      return *this;
    }

    executor &operator=(executor &&other) noexcept {
      if (this != &other) {
        release();
        inner_ = std::move(other.inner_);
        state_ = std::exchange(other.state_, nullptr);
      }

      return *this;
    }

    ~executor() { release(); }

    void post(async::work_item &item) const { inner_.post(item); }

    /** @brief The network of the runtime's loop: its asio executor (see io::network_ref), as long-lived as this. */
    [[nodiscard]] io::network_ref query(async::service_tag<io::network_ref>) const { return io::network_ref{inner_}; }

    /** @brief The timer of the runtime's loop: its asio executor (see async::timer_ref), as long-lived as this. */
    [[nodiscard]] async::timer_ref query(async::service_tag<async::timer_ref>) const {
      return async::timer_ref{inner_};
    }

#if defined(QUOLL_ASIO_TLS)
    /** @brief The TLS network of the runtime's loop: its asio executor, as long-lived as this. */
    [[nodiscard]] io::tls_network_ref query(async::service_tag<io::tls_network_ref>) const {
      return io::tls_network_ref{inner_};
    }
#endif

    [[nodiscard]] bool operator==(executor const &other) const noexcept { return inner_ == other.inner_; }

  private:
    friend class runtime;

    executor(detail::tracked_io_executor inner, detail::runtime_state &state) noexcept
        : inner_(std::move(inner)), state_(&state) {
      ++state_->executors;
    }

    void release() noexcept {
      if (state_ != nullptr && --state_->executors == 0) {
        state_->idle();
      }
    }

    asio::executor<detail::tracked_io_executor> inner_;
    detail::runtime_state *state_;
  };

  using executor_type = executor;

  runtime() : state_(std::make_unique<detail::runtime_state>()) {}

  runtime(runtime const &) = delete;
  runtime(runtime &&) = delete;
  runtime &operator=(runtime const &) = delete;
  runtime &operator=(runtime &&) = delete;
  ~runtime() = default;

  /** @brief An executor of the runtime, to co_spawn() its tasks on. */
  [[nodiscard]] executor get_executor() noexcept {
    return executor{::asio::prefer(state_->context.get_executor(), ::asio::execution::outstanding_work.tracked),
                    *state_};
  }

  /**
   * @brief A stop token that the first SIGINT or SIGTERM (Ctrl-C) stops. After a run() ended by a signal, the next call
   * hands out a fresh token, and waits for the signals again.
   */
  [[nodiscard]] std::stop_token get_stop_token() {
    if (state_->stop.stop_requested()) {
      state_->stop = std::stop_source{};
    }

    if (!state_->signals || state_->signals->released()) {
      state_->signals.reset();
      state_->signals.emplace(state_->context, state_->stop);
    }

    return state_->stop.get_token();
  }

  /** @brief Runs the loop in the calling thread until the runtime's tasks have ended and their work has drained. */
  void run() {
    // Signals with no task to stop would keep the loop running.
    if (state_->executors.load() == 0 && state_->signals) {
      state_->signals->release();
    }

    state_->context.restart();
    state_->context.run();
  }

  /** @brief The io_context, to mix the runtime with asio code. */
  [[nodiscard]] ::asio::io_context &context() noexcept { return state_->context; }

private:
  std::unique_ptr<detail::runtime_state> state_;
};

static_assert(async::Executor<runtime::executor>);
static_assert(async::OffersService<runtime::executor, io::network_ref>);
static_assert(async::OffersService<runtime::executor, async::timer_ref>);
#if defined(QUOLL_ASIO_TLS)
static_assert(async::OffersService<runtime::executor, io::tls_network_ref>);
#endif

} // namespace quoll::asio

#endif // QUOLL_ASIO_RUNTIME_HPP
