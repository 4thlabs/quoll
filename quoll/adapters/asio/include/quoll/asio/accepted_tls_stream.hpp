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

#ifndef QUOLL_ASIO_ACCEPTED_TLS_STREAM_HPP
#define QUOLL_ASIO_ACCEPTED_TLS_STREAM_HPP

// Only with OpenSSL: see QUOLL_TLS in this adapter's CMakeLists.txt.
#if defined(QUOLL_ASIO_TLS)

#include <quoll/asio/tls_stream.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/code_of.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/stopped_as_optional.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/pollable_stream.hpp>

#include <asio/ssl/context.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace quoll::asio {

/**
 * @brief A TLS session a tls_acceptor accepted, as its accept() gives it in an io::any_stream: recover it with
 * `target<asio::accepted_tls_stream>()`, and what OpenSSL negotiated with session().
 *
 * Its handshake, as a server, takes place on its first read, in the connection's own task rather than in the accept
 * loop, where a slow peer would hold every other one back; the timeouts of a read apply to it too. A server that speaks
 * first calls handshake() before writing. A handshake that failed or was stopped leaves the session unusable: it is the
 * error of every later handshake(), read and shutdown_send(), and a write throws it.
 *
 * Move-only. Its state lives on the heap: moving it is cheap, and allowed while an operation is pending; destroying it
 * is not. One handshake at a time, and no read while one is pending. Thread safety: none, as its tls_stream.
 */
class accepted_tls_stream {
public:
  /** @brief Takes over a session on an accepted socket, made from `context`, which it keeps for as long as it lives. */
  accepted_tls_stream(tls_stream stream, std::shared_ptr<::asio::ssl::context> context)
      : state_(std::make_unique<state>(std::move(context), std::move(stream))) {}

  /**
   * @brief Negotiates the session as a server, once: the first read does it if this has not. Reports the error of a
   * handshake that failed or was stopped, again on every later call.
   * @throws std::logic_error while another handshake is pending.
   */
  [[nodiscard]] async::task<std::error_code> handshake() QUOLL_LIFETIMEBOUND { return shake_hands(checked_state()); }

  /**
   * @brief Reads what the peer sent, after the handshake for the first read; a failed handshake is its error.
   * @throws std::logic_error while a handshake is pending.
   */
  [[nodiscard]] async::task<io::io_result> read_some(std::span<std::byte> buffer QUOLL_LIFETIMEBOUND
  ) QUOLL_LIFETIMEBOUND {
    return read(checked_state(), buffer);
  }

  /**
   * @brief Sends some of `data`, once the handshake has completed.
   * @throws std::logic_error before it has; std::system_error with its error once it has failed: a server answers
   * what it has read.
   */
  [[nodiscard]] tls_stream::write_awaitable write_some(std::span<std::byte const> data QUOLL_LIFETIMEBOUND
  ) QUOLL_LIFETIMEBOUND {
    auto &current = checked_state();

    if (current.at != phase::done) {
      throw std::logic_error{"quoll::asio::accepted_tls_stream: written to before its handshake"};
    }

    if (current.failure) {
      throw std::system_error{current.failure, "quoll::asio::accepted_tls_stream: its handshake failed"};
    }

    return current.stream.write_some(data);
  }

  /** @brief Sends TLS's closing message and stops sending: see tls_stream::shutdown_send(). After a failed handshake,
   * its error. */
  [[nodiscard]] async::task<io::io_result> shutdown_send() QUOLL_LIFETIMEBOUND { return shut_down(checked_state()); }

  /** @brief What a read started now would do, once handshaken: see tls_stream::poll_read(). Unknown before. */
  [[nodiscard]] io::read_readiness poll_read() noexcept {
    auto &current = checked_state();

    return handshaken() ? current.stream.poll_read() : io::read_readiness::unknown;
  }

  /** @brief Whether the handshake has completed. */
  [[nodiscard]] bool handshaken() const noexcept {
    auto const &current = checked_state();

    return current.at == phase::done && !current.failure;
  }

  /**
   * @brief The session, for what OpenSSL negotiated once handshaken(): its peer's certificate, its protocol. Reading
   * or writing through it goes around this stream.
   */
  [[nodiscard]] tls_stream::session_type &session() noexcept QUOLL_LIFETIMEBOUND {
    return checked_state().stream.session();
  }

  /** @copydoc session */
  [[nodiscard]] tls_stream::session_type const &session() const noexcept QUOLL_LIFETIMEBOUND {
    return checked_state().stream.session();
  }

private:
  enum class phase : std::uint8_t { before, pending, done };

  struct state {
    state(std::shared_ptr<::asio::ssl::context> made_from, tls_stream accepted) noexcept
        : context(std::move(made_from)), stream(std::move(accepted)) {}

    // Before the session, which is made from it.
    std::shared_ptr<::asio::ssl::context> context;
    tls_stream stream;
    phase at = phase::before;
    // Once done: none, or what made the session unusable.
    std::error_code failure;
  };

  // On the state, bound at the call: the stream may move while they are pending.
  static async::task<std::error_code> shake_hands(state &current) {
    if (current.at == phase::done) {
      co_return current.failure;
    }

    if (current.at == phase::pending) {
      throw std::logic_error{"quoll::asio::accepted_tls_stream: a handshake is already pending"};
    }

    // Stopped before it starts: the session, untouched, can still take one; the operation ends on the stop.
    if ((co_await async::current_stop_token()).stop_requested()) {
      co_await async::detail::end_stopped{};
    }

    current.at = phase::pending;

    try {
      auto shaken = co_await async::stopped_as_optional(current.stream.handshake(tls_stream::session_type::server));

      // Stopped half-way through: the session goes no further, as for a failure, and the operation ends on the stop.
      if (!shaken) {
        current.failure = std::make_error_code(std::errc::operation_canceled);
        current.at = phase::done;
        co_await async::detail::end_stopped{};
      }

      current.failure = *shaken;
    } catch (...) {
      // Whatever it threw, the session is half-way through and goes no further.
      current.failure =
          async::detail::code_of(std::current_exception()).value_or(std::make_error_code(std::errc::io_error));
      current.at = phase::done;
      throw;
    }

    current.at = phase::done;
    co_return current.failure;
  }

  static async::task<io::io_result> read(state &current, std::span<std::byte> buffer) {
    if (current.at != phase::done || current.failure) {
      if (auto const error = co_await shake_hands(current)) {
        co_return io::io_result{error, 0};
      }
    }

    co_return co_await current.stream.read_some(buffer);
  }

  static async::task<io::io_result> shut_down(state &current) {
    if (current.at == phase::done && current.failure) {
      co_return io::io_result{current.failure, 0};
    }

    co_return co_await current.stream.shutdown_send();
  }

  [[nodiscard]] state &checked_state() noexcept {
    assert(state_ && "accepted_tls_stream used after being moved from");
    return *state_;
  }

  [[nodiscard]] state const &checked_state() const noexcept {
    assert(state_ && "accepted_tls_stream used after being moved from");
    return *state_;
  }

  std::unique_ptr<state> state_;
};

static_assert(io::HalfClosableStream<accepted_tls_stream>);
static_assert(io::PollableStream<accepted_tls_stream>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_TLS

#endif // QUOLL_ASIO_ACCEPTED_TLS_STREAM_HPP
