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

#ifndef QUOLL_ASIO_TCP_STREAM_HPP
#define QUOLL_ASIO_TCP_STREAM_HPP

#include <quoll/asio/detail/cancellation.hpp>
#include <quoll/asio/detail/io_error.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/io_result.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

namespace quoll::asio {

namespace detail {

// The cancellation of each direction, allocated together.
struct tcp_cancellations {
  explicit tcp_cancellations(::asio::any_io_executor const &executor) noexcept : read(executor), write(executor) {}

  cancellation read;
  cancellation write;
};

// What a tcp_stream keeps at a stable address: the socket, and the cancellation of each direction.
struct tcp_state {
  explicit tcp_state(::asio::ip::tcp::socket &&connected)
      : socket(std::move(connected)), cancellations(std::make_shared<tcp_cancellations>(socket.get_executor())),
        read(cancellations, &cancellations->read), write(cancellations, &cancellations->write) {}

  ::asio::ip::tcp::socket socket;
  std::shared_ptr<tcp_cancellations> cancellations;
  // Share the ownership of `cancellations`.
  std::shared_ptr<cancellation> read;
  std::shared_ptr<cancellation> write;
};

struct read_some_initiation {
  ::asio::ip::tcp::socket *socket;
  std::span<std::byte> buffer;

  template <class Handler>
  void operator()(Handler &&handler) const {
    socket->async_read_some(::asio::buffer(buffer.data(), buffer.size()), std::forward<Handler>(handler));
  }
};

struct write_some_initiation {
  ::asio::ip::tcp::socket *socket;
  std::span<std::byte const> data;

  template <class Handler>
  void operator()(Handler &&handler) const {
    socket->async_write_some(::asio::buffer(data.data(), data.size()), std::forward<Handler>(handler));
  }
};

} // namespace detail

/**
 * @brief ByteStream over a connected asio TCP socket.
 *
 * Operations complete on the socket's executor, then resume their coroutine on its own executor, which may be
 * another one (a thread pool). A stop request on the awaiter's context, from any thread, cancels the pending
 * operation, which then reports `std::errc::operation_canceled`. The peer closing its side is
 * `io::error::end_of_stream`. Like any asio I/O object, the socket needs an executor that runs one handler at a time:
 * an io_context run by one thread, or a strand.
 *
 * Move-only. The socket lives on the heap: moving the stream is cheap, and allowed while an operation is pending.
 * Destroying it is not, nor destroying its io_context: the waiting coroutine would never resume, and a later stop
 * request would post to a dead executor. Thread safety: none, like the socket.
 */
class tcp_stream {
public:
  /** @brief Awaitable returned by read_some(). */
  class [[nodiscard]] read_awaitable {
  public:
    [[nodiscard]] bool await_ready() const noexcept { return operation_.await_ready(); }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
      return operation_.await_suspend(handle, context);
    }

    // Bytes read along with an error are reported alone: the socket reports the error again on the next read.
    [[nodiscard]] io::io_result await_resume() {
      auto const [error, transferred] = operation_.await_resume();

      if (transferred > 0) {
        return {{}, transferred};
      }

      return {detail::to_io_error(error), 0};
    }

  private:
    friend class tcp_stream;

    using operation = detail::operation<detail::read_some_initiation, std::size_t>;

    explicit read_awaitable(operation &&wrapped) noexcept : operation_(std::move(wrapped)) {}

    operation operation_;
  };

  /** @brief Awaitable returned by write_some(). */
  class [[nodiscard]] write_awaitable {
  public:
    [[nodiscard]] bool await_ready() const noexcept { return operation_.await_ready(); }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
      return operation_.await_suspend(handle, context);
    }

    [[nodiscard]] io::io_result await_resume() {
      auto const [error, transferred] = operation_.await_resume();

      return {detail::to_io_error(error), transferred};
    }

  private:
    friend class tcp_stream;

    using operation = detail::operation<detail::write_some_initiation, std::size_t>;

    explicit write_awaitable(operation &&wrapped) noexcept : operation_(std::move(wrapped)) {}

    operation operation_;
  };

  /** @brief Takes a connected socket over. */
  explicit tcp_stream(::asio::ip::tcp::socket socket)
      : state_(std::make_unique<detail::tcp_state>(std::move(socket))) {}

  /** @brief Reads what the peer sent, waiting for some if nothing arrived yet. */
  [[nodiscard]] read_awaitable read_some(std::span<std::byte> buffer) {
    auto &state = checked_state();

    return read_awaitable{{detail::read_some_initiation{&state.socket, buffer}, state.read}};
  }

  /** @brief Sends some of `data`. */
  [[nodiscard]] write_awaitable write_some(std::span<std::byte const> data) {
    auto &state = checked_state();

    return write_awaitable{{detail::write_some_initiation{&state.socket, data}, state.write}};
  }

  /** @brief The socket, for its options, shutdown() and close(). */
  [[nodiscard]] ::asio::ip::tcp::socket &socket() noexcept { return checked_state().socket; }

private:
  [[nodiscard]] detail::tcp_state &checked_state() const noexcept {
    assert(state_ && "tcp_stream used after being moved from");
    return *state_;
  }

  std::unique_ptr<detail::tcp_state> state_;
};

static_assert(async::ContextAwaitable<tcp_stream::read_awaitable>);
static_assert(async::ContextAwaitable<tcp_stream::write_awaitable>);
static_assert(io::ByteStream<tcp_stream>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_TCP_STREAM_HPP
