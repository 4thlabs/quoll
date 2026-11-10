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

#ifndef QUOLL_HTTP_WEBSOCKET_HPP
#define QUOLL_HTTP_WEBSOCKET_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/http/websocket_parser.hpp>
#include <quoll/io/any_stream.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace quoll::http {

namespace detail {
struct websocket_state;
struct websocket_access;
} // namespace detail

/** @brief What a WebSocket connection accepts from its peer. */
struct websocket_options {
  /** @brief The most a message holds, its fragments joined: a larger one closes the connection with 1009. */
  std::size_t max_message_size = websocket_parser::default_max_message_size;
  /**
   * @brief How long the peer may send nothing before it is pinged, and then before it is taken as gone; zero, never,
   * for a peer the application watches itself, each counted at the end of an interval (see websocket). For a connection
   * http::accept_websocket hands over, all along, a peer gone stopping the handler; for a client's, while a receive()
   * waits, which then throws why; for a server's built by hand, none, the application watching it. For any connection
   * with a timer, how long a write may wait with nothing taken before the peer is taken as gone: this, or 30 seconds
   * where it is zero.
   */
  std::chrono::nanoseconds ping_interval = std::chrono::seconds{30};
};

/**
 * @brief A WebSocket connection, once its handshake is over: messages received and sent in both directions at once
 * (RFC 6455). A server gets one from http::accept_websocket, a client from http::connect_websocket.
 *
 * @code
 * for (;;) {
 *   auto message = co_await socket.receive();
 *
 *   if (message.opcode == http::websocket_opcode::close) {
 *     break;
 *   }
 *
 *   co_await socket.send_text(message.data);
 * }
 * @endcode
 *
 * receive() gives each text or binary message the peer sends, then the close that ends them. It answers pings with
 * pongs, drops pongs, and answers the peer's close with its own, so that a loop that receives keeps the connection
 * alive. A peer that breaks the protocol, or a message larger than websocket_options::max_message_size, is sent the
 * close the failure calls for (see close_code_of()), and receive() throws it.
 *
 * Sending goes on while a receive() waits, from a task beside it through when_all: messages leave whole, one at a
 * time, in the order their tasks start. Once a close has gone out, by close() or as the answer to the peer's, a
 * message throws std::system_error with `http::error::websocket_closed`, as does all that follows a frame cut short,
 * by a stop or a failure of the stream; a close() then does nothing.
 *
 * Given a timer, a write that waits websocket_options::ping_interval, or 30 seconds where it is zero, with nothing
 * taken has the peer taken as gone: its frame is cut, and the send throws std::errc::timed_out. A pong, and the close
 * that answers the peer's, go whole once they have the turn to send; a receive() stopped before then leaves them
 * unsent. A receive() stopped, or failed, ends receiving: a stream such as TLS does not take a read up again once it
 * was stopped, so the next one throws the same. Without a timer, nothing here times out.
 *
 * A client's connection, as http::connect_websocket gives one, keeps itself alive while a receive() waits, as a
 * server's does beside its handler, given a timer, with one keep-alive at a time,
 * started by a receive() as it first reads and ending once none reads: at the end of each ping_interval, a peer that
 * sent nothing in it is pinged, and one that sent nothing since its ping left, or was dropped as sending had ended, or
 * since a close has left, is taken as gone, so that a peer silent since just after the end of one is pinged within two
 * intervals and taken as gone within three, but for the time its ping waits for the turn behind a send or takes to
 * leave, as the next interval starts once it has. The receive() then throws std::errc::timed_out, the failure of the
 * ping that could not leave, or io_error for a keep-alive that failed, and so do every one after it that has to read
 * and every send, but one after the connection's own close, which says http::error::websocket_closed as before, a
 * frame that comes at the very end of the interval included, which Beast takes as late too. The ping
 * goes on its own: a receive() waits for it only to send a pong or a close of its own, which take their turn behind it.
 *
 * Copied, not moved: its copies are the same connection, as http::client's are, which a task receiving and another
 * sending may each hold. An operation holds what it works on from when it is called, so that every copy may go before
 * it ends: the stream closes once the last of them, and of the operations, has. One receive() at a time, across copies,
 * another asked while one is pending throwing std::logic_error, in every build.
 *
 * Thread safety: that of the stream, which an asio socket does not share between threads. What a receive() and a send
 * do, and a client's keep-alive, which runs on the executor of the receive() that started it, takes turns as long as
 * they all run on one executor that runs a handler at a time, a strand where it runs on several threads: the strand of
 * its own a server gives each connection, where its handler runs, or an executor of quoll::runtime, a strand on
 * several threads, where a client runs, and the tasks they await through when_all. A task on another executor, one
 * that others send through to broadcast say, would reach the stream at once: what others want sent goes to the
 * handler, which sends it itself.
 */
class websocket {
public:
  /**
   * @brief The connection over `stream`, for `role`'s end, its handshake over, with `received` what came on it after
   * the handshake.
   */
  websocket(io::any_stream stream, websocket_role role, std::string received = {}, websocket_options options = {});

  websocket(websocket const &) noexcept = default;
  websocket &operator=(websocket const &) noexcept = default;
  ~websocket() = default;

  /**
   * @brief The next text or binary message, or the peer's close (`opcode` close, with its code and reason), the last.
   *
   * @throws std::system_error with an http::error for a peer that breaks the protocol, the stream's error, or, for a
   * peer the keep-alive took as gone, std::errc::timed_out or why its ping could not leave (see websocket), and
   * std::logic_error asked again after the close, or while another receive() is pending, through any copy.
   */
  [[nodiscard]] async::task<websocket_message> receive();

  /** @brief Sends `text`, copied. @throws std::invalid_argument, before the task runs, for text that is not UTF-8. */
  [[nodiscard]] async::task<> send_text(std::string_view text);

  /** @brief Sends `data`, copied. */
  [[nodiscard]] async::task<> send_binary(std::string_view data);

  /**
   * @brief Sends a ping, with `data`, copied, which the peer's pong carries back.
   *
   * @throws std::invalid_argument, before the task runs, for data longer than 125 bytes.
   */
  [[nodiscard]] async::task<> ping(std::string_view data = {});

  /**
   * @brief Sends a close with `code` and `reason`, copied, unless one was sent: nothing is sent after it. The peer's
   * answer comes to receive(), as the last message.
   *
   * @throws std::invalid_argument, before the task runs, for what format_websocket_close() refuses.
   */
  [[nodiscard]] async::task<> close(std::uint16_t code = 1000, std::string_view reason = {});

private:
  friend struct detail::websocket_access;

  std::shared_ptr<detail::websocket_state> state_;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_WEBSOCKET_HPP
