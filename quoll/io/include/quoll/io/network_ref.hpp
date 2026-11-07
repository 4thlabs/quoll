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

#ifndef QUOLL_IO_NETWORK_REF_HPP
#define QUOLL_IO_NETWORK_REF_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/network.hpp>

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace quoll::io {

/**
 * @brief Non-owning, type-erased reference to a Network; models Network itself. The service an executor of an I/O
 * loop offers: `executor.query<io::network_ref>()`.
 *
 * Trivially copyable and never allocates. The referenced network, usually the executor that answered the query, must
 * outlive the reference: io::connect() and io::listen() use it at once, from the task's own executor.
 *
 * Thread safety: same as the referenced network.
 */
class network_ref {
public:
  // The exclusion comes first: it stops Network<network_ref> from checking this constructor against itself.
  template <class N>
    requires(!std::same_as<N, network_ref> && Network<N>)
  explicit network_ref(N const &network) noexcept : network_(&network), vtable_(&vtable_for<N>) {}

  template <class N>
    requires(!std::same_as<N, network_ref> && Network<N>)
  explicit network_ref(N const &&network) = delete;

  /** @brief Opens a connection to `host` (a name or an address) on `port`. */
  [[nodiscard]] async::task<any_stream> connect(std::string host, std::uint16_t port) const {
    return vtable_->connect(network_, std::move(host), port);
  }

  /** @brief Listens on `address` (an IP address) and `port` (0 for any free port). */
  [[nodiscard]] any_listener listen(std::string address, std::uint16_t port) const {
    return vtable_->listen(network_, std::move(address), port);
  }

  /** @brief The referenced network if its type is exactly `N`, otherwise `nullptr`. */
  template <class N>
  [[nodiscard]] N const *target() const noexcept {
    return vtable_->type == async::detail::type_id_of<N>() ? static_cast<N const *>(network_) : nullptr;
  }

private:
  struct vtable {
    async::task<any_stream> (*connect)(void const *network, std::string host, std::uint16_t port);
    any_listener (*listen)(void const *network, std::string address, std::uint16_t port);
    async::detail::type_id type;
  };

  template <Network N>
  static constexpr vtable vtable_for{
      // A coroutine, so that `host` stays until its task has ended: it may take a view of it.
      [](void const *network, std::string host, std::uint16_t port) -> async::task<any_stream> {
        co_return co_await static_cast<N const *>(network)->connect(host, port);
      },
      [](void const *network, std::string address, std::uint16_t port) {
        return static_cast<N const *>(network)->listen(std::move(address), port);
      },
      async::detail::type_id_of<N>(),
  };

  void const *network_;
  vtable const *vtable_;
};

static_assert(Network<network_ref>);
static_assert(std::is_trivially_copyable_v<network_ref>);

} // namespace quoll::io

#endif // QUOLL_IO_NETWORK_REF_HPP
