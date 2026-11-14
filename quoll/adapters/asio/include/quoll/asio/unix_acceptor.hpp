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

#ifndef QUOLL_ASIO_UNIX_ACCEPTOR_HPP
#define QUOLL_ASIO_UNIX_ACCEPTOR_HPP

#if defined(QUOLL_ASIO_LOCAL)

#include <quoll/asio/detail/accept.hpp>
#include <quoll/asio/unix_stream.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/listener.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/local/stream_protocol.hpp>

#include <cassert>
#include <memory>
#include <optional>
#include <utility>

namespace quoll::asio {

/**
 * @brief Listener accepting connections on a Unix domain socket with asio, as acceptor does on TCP: each a unix_stream,
 * handed over as an io::any_stream, on a strand of its own when `socket_strands` is given.
 *
 * Move-only, the acceptor on the heap. The socket's file stays when it goes. One accept at a time. Thread safety: none.
 */
class unix_acceptor {
public:
  /** @brief Takes a listening acceptor over, each accepted socket on a strand made from `socket_strands`, if any. */
  explicit unix_acceptor(
      ::asio::local::stream_protocol::acceptor listening, std::optional<::asio::any_io_executor> socket_strands = {}
  )
      : state_(std::make_unique<state_type>(std::move(listening), std::move(socket_strands))) {}

  unix_acceptor(unix_acceptor const &) = delete;
  unix_acceptor &operator=(unix_acceptor const &) = delete;
  unix_acceptor(unix_acceptor &&) noexcept = default;
  unix_acceptor &operator=(unix_acceptor &&) noexcept = default;
  ~unix_acceptor() = default;

  /** @brief The next connection a peer opens. */
  [[nodiscard]] async::task<io::any_stream> accept() { return accepting(checked_state()); }

  /** @brief The listening socket, for its options and local_endpoint(). */
  [[nodiscard]] ::asio::local::stream_protocol::acceptor &socket() noexcept { return checked_state().acceptor; }

private:
  using state_type = detail::basic_acceptor_state<::asio::local::stream_protocol>;

  // On the state, bound at the call: the acceptor may move before its task runs.
  static async::task<io::any_stream> accepting(state_type &state) {
    co_return io::any_stream{unix_stream{co_await detail::accept_socket(state)}};
  }

  [[nodiscard]] state_type &checked_state() noexcept {
    assert(state_ && "unix_acceptor used after being moved from");
    return *state_;
  }

  std::unique_ptr<state_type> state_;
};

static_assert(io::Listener<unix_acceptor>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_LOCAL

#endif // QUOLL_ASIO_UNIX_ACCEPTOR_HPP
