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

#include <quoll/asio/detail/loop_services.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/offers_service.hpp>
#include <quoll/async/offloader_ref.hpp>
#include <quoll/async/service.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/io/network_ref.hpp>
#if defined(QUOLL_ASIO_TLS)
#include <quoll/io/tls_network_ref.hpp>
#endif

#include <memory>
#include <stop_token>
#include <utility>

// Declared rather than included: a program using the runtime does not compile asio.
namespace asio {
class io_context;
} // namespace asio

namespace quoll::asio {

/**
 * @brief The asio runtime: an I/O loop run by the thread calling run(), on which a program's tasks run.
 *
 * @code
 * quoll::async::task<> serve_notes(std::uint16_t port) {
 *   quoll::http::router router;
 *
 *   quoll::rest::mount<notes::api>(router, notes_service{});
 *   co_await quoll::http::server{router}.listen(port);
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
 * io::listen(), async::sleep_for() and what is built on them (quoll::http::server, quoll::http::client, a REST
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
 * Neither copied nor moved: its executors point to it, and it outlives them. Thread safety: run() and get_stop_token()
 * on one thread; get_executor() and the executors from any thread.
 */
class runtime {
public:
  struct state;

  /** @brief The executor of the runtime: posts to its loop, and offers its network, its timer and, with OpenSSL, TLS.
   */
  class executor {
  public:
    using services = detail::loop_services;

    void post(async::work_item &item) const;

    /** @brief The network of the runtime's loop, as long-lived as the runtime. */
    [[nodiscard]] io::network_ref query(async::service_tag<io::network_ref>) const;

    /** @brief The timer of the runtime's loop, as long-lived as the runtime. */
    [[nodiscard]] async::timer_ref query(async::service_tag<async::timer_ref>) const;

    /** @brief What runs blocking work away from the loop, as long-lived as the runtime. */
    [[nodiscard]] async::offloader_ref query(async::service_tag<async::offloader_ref>) const;

#if defined(QUOLL_ASIO_TLS)
    /** @brief The TLS network of the runtime's loop, as long-lived as the runtime. */
    [[nodiscard]] io::tls_network_ref query(async::service_tag<io::tls_network_ref>) const;
#endif

    [[nodiscard]] bool operator==(executor const &other) const noexcept { return state_ == other.state_; }

  private:
    friend class runtime;

    executor(state &shared, std::shared_ptr<void> work) noexcept : state_(&shared), work_(std::move(work)) {}

    state *state_;
    // Keeps the loop running: shared by the executors of the runtime, and gone with the last one.
    std::shared_ptr<void> work_;
  };

  using executor_type = executor;

  runtime();

  runtime(runtime const &) = delete;
  runtime(runtime &&) = delete;
  runtime &operator=(runtime const &) = delete;
  runtime &operator=(runtime &&) = delete;
  ~runtime();

  /** @brief An executor of the runtime, to co_spawn() its tasks on. */
  [[nodiscard]] executor get_executor();

  /**
   * @brief A stop token that the first SIGINT or SIGTERM (Ctrl-C) stops. After a run() ended by a signal, the next call
   * hands out a fresh token, and waits for the signals again.
   */
  [[nodiscard]] std::stop_token get_stop_token();

  /** @brief Runs the loop in the calling thread until the runtime's tasks have ended and their work has drained. */
  void run();

  /**
   * @brief The io_context, to mix the runtime with asio code, which then includes asio itself: the same version as this
   * library was compiled with, as a different one would not be the same class.
   */
  [[nodiscard]] ::asio::io_context &context() noexcept;

private:
  std::unique_ptr<state> state_;
};

static_assert(async::Executor<runtime::executor>);
static_assert(async::OffersService<runtime::executor, io::network_ref>);
static_assert(async::OffersService<runtime::executor, async::timer_ref>);
static_assert(async::OffersService<runtime::executor, async::offloader_ref>);
#if defined(QUOLL_ASIO_TLS)
static_assert(async::OffersService<runtime::executor, io::tls_network_ref>);
#endif

} // namespace quoll::asio

#endif // QUOLL_ASIO_RUNTIME_HPP
