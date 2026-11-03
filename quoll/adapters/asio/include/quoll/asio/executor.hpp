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

#include <quoll/asio/detail/network.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/service.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/io/network.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/post.hpp>

#include <concepts>
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
 * Offers the network of its execution context (io::network): io::connect() and io::listen() open TCP connections
 * and listening sockets there, for the tasks it runs.
 *
 * Thread safety: post() may be called from any thread, as asio's post can.
 */
template <class AsioExecutor = ::asio::any_io_executor>
  requires std::copy_constructible<AsioExecutor> && std::equality_comparable<AsioExecutor>
class executor {
public:
  using services = async::service_list<io::network>;

  explicit executor(AsioExecutor inner) noexcept(std::is_nothrow_move_constructible_v<AsioExecutor>)
      : inner_(std::move(inner)) {}

  void post(async::work_item &item) const { ::asio::post(inner_, resumer{&item}); }

  /** @brief The network of the execution context, valid while this executor lives. */
  [[nodiscard]] io::network query(async::service_tag<io::network>) const {
    return io::network{detail::network<AsioExecutor>{&inner_}};
  }

  [[nodiscard]] bool operator==(executor const &) const noexcept = default;

private:
  struct resumer {
    async::work_item *item;

    // The handle is read before the coroutine runs: it may destroy the item.
    void operator()() const { item->handle.resume(); }
  };

  AsioExecutor inner_;
};

static_assert(async::Executor<executor<>>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_EXECUTOR_HPP
