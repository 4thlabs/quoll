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

#include <quoll/async/task.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/network.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace quoll::io {

/**
 * @brief The network service of an executor, `executor.query<io::network_ref>()`, as an asio executor offers it.
 *
 * A type-erased reference to a Network: valid while the executor that handed it out lives and stays where it is, like
 * a std::string_view. Use it at once, from the task that got it; io::connect() and io::listen() do.
 */
class network_ref {
public:
  template <Network N>
    requires(sizeof(N) <= 2 * sizeof(void *) && alignof(N) <= alignof(void *))
  explicit network_ref(N network) noexcept : vtable_(&vtable_for<N>) {
    std::construct_at(reinterpret_cast<N *>(storage_), network);
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
  [[nodiscard]] static N const &held(void const *storage) noexcept {
    return *std::launder(static_cast<N const *>(storage));
  }

  template <Network N>
  static constexpr vtable vtable_for{
      [](void const *network, std::string host, std::uint16_t port) {
        return held<N>(network).connect(std::move(host), port);
      },
      [](void const *network, std::string address, std::uint16_t port) {
        return held<N>(network).listen(std::move(address), port);
      },
  };

  alignas(void *) std::byte storage_[2 * sizeof(void *)];
  vtable const *vtable_;
};

} // namespace quoll::io

#endif // QUOLL_IO_NETWORK_REF_HPP
