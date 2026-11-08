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

#ifndef QUOLL_ASIO_DETAIL_BASIC_STREAM_HPP
#define QUOLL_ASIO_DETAIL_BASIC_STREAM_HPP

#include <quoll/asio/detail/cancellation.hpp>
#include <quoll/asio/detail/io_error.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/async/await_context.hpp>
#include <quoll/io/io_result.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/buffer.hpp>

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

namespace quoll::asio::detail {

// The cancellation of each direction, allocated together.
struct stream_cancellations {
  explicit stream_cancellations(::asio::any_io_executor const &executor) noexcept : read(executor), write(executor) {}

  cancellation read;
  cancellation write;
};

// What a stream keeps at a stable address: the asio stream, and the cancellation of each direction.
template <class Stream>
struct stream_state {
  explicit stream_state(Stream &&connected)
      : stream(std::move(connected)), cancellations(std::make_shared<stream_cancellations>(stream.get_executor())),
        read(cancellations, &cancellations->read), write(cancellations, &cancellations->write) {}

  Stream stream;
  std::shared_ptr<stream_cancellations> cancellations;
  // Share the ownership of `cancellations`.
  std::shared_ptr<cancellation> read;
  std::shared_ptr<cancellation> write;
  // Set once TLS's closing message has been made, for it to go once, whatever becomes of its write or of the socket's
  // shutdown after it. Read and written on the stream's executor only, in the initiations.
  bool closing_sent = false;
};

template <class Stream>
struct read_some_initiation {
  Stream *stream;
  std::span<std::byte> buffer;

  template <class Handler>
  void operator()(Handler &&handler) const {
    stream->async_read_some(::asio::buffer(buffer.data(), buffer.size()), std::forward<Handler>(handler));
  }
};

template <class Stream>
struct write_some_initiation {
  Stream *stream;
  std::span<std::byte const> data;

  template <class Handler>
  void operator()(Handler &&handler) const {
    stream->async_write_some(::asio::buffer(data.data(), data.size()), std::forward<Handler>(handler));
  }
};

// What a ByteStream over an asio stream is, whatever the stream: a socket, a TLS session.
//
// Move-only. The asio stream lives on the heap: moving is cheap, and allowed while an operation is pending.
template <class Stream>
class basic_stream {
public:
  /** @brief Awaitable returned by read_some(). */
  class [[nodiscard]] read_awaitable {
  public:
    [[nodiscard]] bool await_ready() const noexcept { return operation_.await_ready(); }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
      return operation_.await_suspend(handle, context);
    }

    // Bytes read along with an error are reported alone: the stream reports the error again on the next read.
    [[nodiscard]] io::io_result await_resume() {
      auto const [error, transferred] = operation_.await_resume();

      if (transferred > 0) {
        return {{}, transferred};
      }

      return {to_io_error(error), 0};
    }

  private:
    friend class basic_stream;

    using wrapped_operation = operation<read_some_initiation<Stream>, std::size_t>;

    explicit read_awaitable(wrapped_operation &&wrapped) noexcept : operation_(std::move(wrapped)) {}

    wrapped_operation operation_;
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

      return {to_io_error(error), transferred};
    }

  private:
    friend class basic_stream;

    using wrapped_operation = operation<write_some_initiation<Stream>, std::size_t>;

    explicit write_awaitable(wrapped_operation &&wrapped) noexcept : operation_(std::move(wrapped)) {}

    wrapped_operation operation_;
  };

  /** @brief Reads what the peer sent, waiting for some if nothing arrived yet. */
  [[nodiscard]] read_awaitable read_some(std::span<std::byte> buffer) {
    auto &state = checked_state();

    return read_awaitable{{read_some_initiation<Stream>{&state.stream, buffer}, state.read}};
  }

  /** @brief Sends some of `data`. */
  [[nodiscard]] write_awaitable write_some(std::span<std::byte const> data) {
    auto &state = checked_state();

    return write_awaitable{{write_some_initiation<Stream>{&state.stream, data}, state.write}};
  }

protected:
  explicit basic_stream(Stream stream) : state_(std::make_unique<stream_state<Stream>>(std::move(stream))) {}

  [[nodiscard]] stream_state<Stream> &checked_state() noexcept {
    assert(state_ && "stream used after being moved from");
    return *state_;
  }

  [[nodiscard]] stream_state<Stream> const &checked_state() const noexcept {
    assert(state_ && "stream used after being moved from");
    return *state_;
  }

private:
  std::unique_ptr<stream_state<Stream>> state_;
};

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_BASIC_STREAM_HPP
