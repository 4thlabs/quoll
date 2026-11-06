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

#ifndef QUOLL_ASIO_ACCEPTOR_HPP
#define QUOLL_ASIO_ACCEPTOR_HPP

#include <quoll/asio/detail/accept.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/bound_listener.hpp>

#include <asio/ip/tcp.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>

namespace quoll::asio {

/**
 * @brief Listener accepting TCP connections with asio, for a server (quoll::http::server).
 *
 * Takes a listening acceptor over. Each accepted connection is a tcp_stream on the acceptor's executor, handed over as
 * an io::any_stream. A failure to accept throws std::system_error from the task, with asio's error code; a stop
 * request, from any thread, abandons the wait, with `std::errc::operation_canceled`. The executor runs one handler at
 * a time (an io_context run by one thread, or a strand).
 *
 * Move-only; the acceptor lives on the heap, so moving it is allowed while an accept is pending. Destroying it, or its
 * io_context, is not. One accept at a time. Thread safety: none, like the acceptor.
 */
class acceptor {
public:
  /** @brief Takes a listening acceptor over. */
  explicit acceptor(::asio::ip::tcp::acceptor listening)
      : state_(std::make_unique<detail::acceptor_state>(std::move(listening))) {}

  /** @brief The next connection a peer opens. */
  [[nodiscard]] async::task<io::any_stream> accept() { return detail::accept(checked_state()); }

  /** @brief The port it accepts on: the one the system picked when it was bound to port 0. */
  [[nodiscard]] std::uint16_t local_port() const { return checked_state().acceptor.local_endpoint().port(); }

  /** @brief The listening socket, for its options and local_endpoint(). */
  [[nodiscard]] ::asio::ip::tcp::acceptor &socket() noexcept { return checked_state().acceptor; }

private:
  [[nodiscard]] detail::acceptor_state &checked_state() const noexcept {
    assert(state_ && "acceptor used after being moved from");
    return *state_;
  }

  std::unique_ptr<detail::acceptor_state> state_;
};

static_assert(io::BoundListener<acceptor>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_ACCEPTOR_HPP
