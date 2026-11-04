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

#ifndef QUOLL_ASIO_EXECUTOR_HPP
#define QUOLL_ASIO_EXECUTOR_HPP

#include <quoll/asio/acceptor.hpp>
#include <quoll/asio/detail/connect.hpp>
#include <quoll/asio/detail/wait.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/offers_service.hpp>
#include <quoll/async/service.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/timer.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/network.hpp>
#include <quoll/io/network_ref.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/execution/outstanding_work.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/prefer.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace quoll::asio {

/**
 * @brief Executor resuming coroutines on an asio executor: an io_context, a strand, any_io_executor.
 *
 * post() hands asio a small function object resuming the work item. Unlike run_loop and the thread pool, asio
 * allocates it (from a per-thread cache once warm), so post() may throw std::bad_alloc; see async::Executor.
 * Equal when the asio executors are.
 *
 * It is also the network of its execution context (io::Network), and offers itself as such (io::network_ref):
 * io::connect() and io::listen() open TCP connections and listening sockets there, for the tasks it runs. Likewise, it
 * is the timer of its execution context (async::Timer), for async::sleep_for() and async::with_timeout().
 *
 * Thread safety: post() may be called from any thread, as asio's post can.
 */
template <class AsioExecutor = ::asio::any_io_executor>
  requires std::copy_constructible<AsioExecutor> && std::equality_comparable<AsioExecutor>
class executor {
public:
  using services = async::service_list<io::network_ref, async::timer_ref>;

  explicit executor(AsioExecutor inner) noexcept(std::is_nothrow_move_constructible_v<AsioExecutor>)
      : inner_(std::move(inner)) {}

  void post(async::work_item &item) const { ::asio::post(inner_, resumer{&item}); }

  /** @brief Opens a TCP connection to `host` (a name or an address) on `port`, on the execution context. */
  [[nodiscard]] async::task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    return detail::connect(io_executor(), std::move(host), port);
  }

  /** @brief Listens on `address` (an IP address) and `port` (0 for any free port), on the execution context. */
  [[nodiscard]] io::any_listener listen(std::string address, std::uint16_t port) const {
    ::asio::ip::tcp::endpoint const endpoint{::asio::ip::make_address(address), port};

    return io::any_listener{acceptor{::asio::ip::tcp::acceptor{io_executor(), endpoint}}};
  }

  /** @brief This executor, as the network of its execution context: valid while it lives (see io::network_ref). */
  [[nodiscard]] io::network_ref query(async::service_tag<io::network_ref>) const { return io::network_ref{*this}; }

  /** @brief Completes with true once `duration` has elapsed, with false on a stop request (see async::Timer). */
  [[nodiscard]] async::task<bool> wait_for(std::chrono::nanoseconds duration) const {
    return detail::wait(io_executor(), duration);
  }

  /** @brief This executor, as the timer of its execution context: valid while it lives (see async::timer_ref). */
  [[nodiscard]] async::timer_ref query(async::service_tag<async::timer_ref>) const { return async::timer_ref{*this}; }

  [[nodiscard]] bool operator==(executor const &) const noexcept = default;

private:
  // The executor of the sockets, acceptors and timers: not counted as work, as an idle connection kept for later is
  // none (a runtime counts the work of its tasks itself; a pending wait counts on its own).
  [[nodiscard]] ::asio::any_io_executor io_executor() const {
    return ::asio::prefer(::asio::any_io_executor{inner_}, ::asio::execution::outstanding_work.untracked);
  }

  struct resumer {
    async::work_item *item;

    // The handle is read before the coroutine runs: it may destroy the item.
    void operator()() const { item->handle.resume(); }
  };

  AsioExecutor inner_;
};

static_assert(async::Executor<executor<>>);
static_assert(io::Network<executor<>>);
static_assert(async::Timer<executor<>>);
static_assert(async::OffersService<executor<>, io::network_ref>);
static_assert(async::OffersService<executor<>, async::timer_ref>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_EXECUTOR_HPP
