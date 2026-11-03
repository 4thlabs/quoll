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

#ifndef QUOLL_IO_NETWORK_HPP
#define QUOLL_IO_NETWORK_HPP

#include <quoll/async/current_executor.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace quoll::io {

/**
 * @brief A network: TCP connections out (`connect`) and in (`listen`), as an I/O loop provides them.
 *
 * A small, trivially copyable handle on the loop, valid while the executor that handed it out lives. `connect(host,
 * port)` behaves as a Connector; `listen(address, port)` binds a listening socket on `address` (an IP address) and
 * `port` (0 for any free port), or throws std::system_error.
 */
template <class N>
concept Network = std::is_trivially_copyable_v<N> && requires(N const &network, std::string text, std::uint16_t port) {
  { network.connect(text, port) } -> std::same_as<async::task<any_stream>>;
  { network.listen(text, port) } -> std::same_as<any_listener>;
};

/**
 * @brief The network service of an executor: `executor.query<io::network>()`, as an asio executor offers it.
 *
 * Type-erased handle on a Network, as small and as short-lived: use it at once, from the task that got it.
 */
class network {
public:
  template <Network N>
    requires(sizeof(N) <= 2 * sizeof(void *) && alignof(N) <= alignof(void *))
  explicit network(N handle) noexcept : vtable_(&vtable_for<N>) {
    std::construct_at(reinterpret_cast<N *>(storage_), handle);
  }

  /** @brief Opens a connection to `host` (a name or an address) on `port`. */
  [[nodiscard]] async::task<any_stream> connect(std::string host, std::uint16_t port) const {
    return vtable_->connect(storage_, std::move(host), port);
  }

  /** @brief Listens on `address` (an IP address) and `port` (0 for any free port). */
  [[nodiscard]] any_listener listen(std::string address, std::uint16_t port) const {
    return vtable_->listen(storage_, std::move(address), port);
  }

private:
  struct vtable {
    async::task<any_stream> (*connect)(void const *network, std::string host, std::uint16_t port);
    any_listener (*listen)(void const *network, std::string address, std::uint16_t port);
  };

  template <Network N>
  static constexpr vtable vtable_for{
      [](void const *network, std::string host, std::uint16_t port) {
        return static_cast<N const *>(network)->connect(std::move(host), port);
      },
      [](void const *network, std::string address, std::uint16_t port) {
        return static_cast<N const *>(network)->listen(std::move(address), port);
      },
  };

  alignas(void *) std::byte storage_[2 * sizeof(void *)];
  vtable const *vtable_;
};

namespace detail {

// The network of the executor the awaiting task runs on.
[[nodiscard]] inline network current_network(async::executor_ref executor, char const *caller) {
  auto found = executor.query<network>();

  if (!found) {
    throw std::logic_error{std::string{caller} + ": the executor offers no network (run the task with sync_wait)"};
  }

  return *found;
}

} // namespace detail

/**
 * @brief Opens a connection to `host` (a name or an address) on `port`, through the network of the executor the task
 * runs on.
 *
 * Fails like a Connector; throws std::logic_error if the executor offers no network.
 */
[[nodiscard]] inline async::task<any_stream> connect(std::string host, std::uint16_t port) {
  auto const network = detail::current_network(co_await async::current_executor(), "quoll::io::connect");

  co_return co_await network.connect(std::move(host), port);
}

/**
 * @brief Listens on `port` (0 for any free port) and `address` (an IP address; loopback by default, `0.0.0.0` for
 * every interface), through the network of the executor the task runs on.
 *
 * Throws std::system_error when the address cannot be bound, std::logic_error if the executor offers no network.
 */
[[nodiscard]] inline async::task<any_listener> listen(std::uint16_t port, std::string address = "127.0.0.1") {
  auto const network = detail::current_network(co_await async::current_executor(), "quoll::io::listen");

  co_return network.listen(std::move(address), port);
}

/** @brief Connector opening its connections with io::connect(): through the network of the executor. */
struct network_connector {
  [[nodiscard]] async::task<any_stream> connect(std::string host, std::uint16_t port) const {
    return io::connect(std::move(host), port);
  }
};

static_assert(Connector<network_connector>);

} // namespace quoll::io

#endif // QUOLL_IO_NETWORK_HPP
