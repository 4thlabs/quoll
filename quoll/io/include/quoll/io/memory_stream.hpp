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

#ifndef QUOLL_IO_MEMORY_STREAM_HPP
#define QUOLL_IO_MEMORY_STREAM_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/chain_owner.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/detail/memory_channel.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/pollable_stream.hpp>

#include <array>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>

namespace quoll::io {

/**
 * @brief One end of an in-memory byte pipe, made by make_memory_stream_pair(): what one end writes, the other
 * reads. For tests, and for connecting two components of the same process.
 *
 * Writes never wait (the pipe is unbounded) and a read waits until the peer writes or closes; the reader resumes
 * on its own executor. A stop request on the reader's await_context ends a waiting read, and its awaiter, on the stop:
 * it does not resume. Destroying an end closes it, and the peer's writes then fail with
 * `std::errc::broken_pipe`. An end must not be destroyed or moved while one of its reads is pending.
 *
 * Move-only. Thread safety: none; both ends, and the stop requests reaching their reads, are used from one thread.
 */
class memory_stream {
  using channels = std::array<detail::memory_channel, 2>;

public:
  /** @brief Awaitable returned by read_some(). */
  class [[nodiscard]] read_awaitable {
  public:
    read_awaitable(read_awaitable const &) = delete;
    read_awaitable &operator=(read_awaitable const &) = delete;
    read_awaitable &operator=(read_awaitable &&) = delete;

    // Moved into the awaiting coroutine before it suspends, when no stop callback is registered yet.
    read_awaitable(read_awaitable &&other) noexcept : channel_(other.channel_), buffer_(other.buffer_) {}

    // A coroutine suspended here may go, and the peer's next write must not then find it. One already woken is on its
    // executor's queue, from where nothing can take it: it would resume a frame that is gone, so this ends the program
    // now rather than then.
    ~read_awaitable() {
      if (!suspended_) {
        return;
      }

      stop_callback_.reset();

      if (channel_->woken()) {
        std::terminate();
      }

      channel_->end_wait();
    }

    [[nodiscard]] bool await_ready() const noexcept { return buffer_.empty() || channel_->readable(); }

    // Stopped already, it ends on the stop before it waits, and touches nothing after: the owner may unwind its frame
    // at once on another thread.
    void await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
      if (context.stop_token.stop_requested()) {
        async::detail::stop_chain(context.owner);
        return;
      }

      item_.handle = handle;
      chain_ = context.owner;
      channel_->wait(item_, context.executor);
      suspended_ = true;

      // Invoked at once if stop was requested since.
      if (context.stop_token.stop_possible()) {
        stop_callback_.emplace(context.stop_token, canceller{this});
      }
    }

    [[nodiscard]] io_result await_resume() noexcept {
      stop_callback_.reset();
      channel_->end_wait();
      suspended_ = false;
      return channel_->read(buffer_);
    }

  private:
    friend class memory_stream;

    // Ends the read on the stop: it does not resume, and the owner of its chain unwinds it. Nothing of it is touched
    // after.
    struct canceller {
      read_awaitable *self;

      // Its wait over: the unwinding then leaves the channel alone, which a read started since may wait on.
      void operator()() const noexcept {
        if (self->channel_->give_up()) {
          self->suspended_ = false;
          async::detail::stop_chain(self->chain_);
        }
      }
    };

    read_awaitable(detail::memory_channel &channel, std::span<std::byte> buffer) noexcept
        : channel_(&channel), buffer_(buffer) {}

    detail::memory_channel *channel_;
    std::span<std::byte> buffer_;
    async::work_item item_;
    async::detail::chain_owner *chain_ = nullptr;
    bool suspended_ = false;
    std::optional<std::stop_callback<canceller>> stop_callback_;
  };

  /** @brief Awaitable returned by write_some(). Completes without suspending. */
  class [[nodiscard]] write_awaitable {
  public:
    [[nodiscard]] bool await_ready() const noexcept { return true; }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<>, async::await_context const &) const noexcept {
      return false;
    }

    [[nodiscard]] io_result await_resume() { return channel_->write(data_); }

  private:
    friend class memory_stream;

    write_awaitable(detail::memory_channel &channel, std::span<std::byte const> data) noexcept
        : channel_(&channel), data_(data) {}

    detail::memory_channel *channel_;
    std::span<std::byte const> data_;
  };

  /** @brief Awaitable returned by shutdown_send(). Completes without suspending. */
  class [[nodiscard]] shutdown_awaitable {
  public:
    [[nodiscard]] bool await_ready() const noexcept { return true; }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<>, async::await_context const &) const noexcept {
      return false;
    }

    [[nodiscard]] io_result await_resume() {
      channel_->close();
      return {};
    }

  private:
    friend class memory_stream;

    explicit shutdown_awaitable(detail::memory_channel &channel) noexcept : channel_(&channel) {}

    detail::memory_channel *channel_;
  };

  memory_stream(memory_stream const &) = delete;
  memory_stream &operator=(memory_stream const &) = delete;

  memory_stream(memory_stream &&other) noexcept : channels_(std::move(other.channels_)), side_(other.side_) {}

  memory_stream &operator=(memory_stream &&other) noexcept {
    if (this != &other) {
      release();
      channels_ = std::move(other.channels_);
      side_ = other.side_;
    }

    return *this;
  }

  ~memory_stream() { release(); }

  /** @brief Reads the bytes the peer wrote, waiting for some if there are none yet. */
  [[nodiscard]] read_awaitable read_some(std::span<std::byte> buffer QUOLL_LIFETIMEBOUND) noexcept QUOLL_LIFETIMEBOUND {
    return {inbound(), buffer};
  }

  /** @brief Whether bytes or the end of the stream wait to be read: see PollableStream. */
  [[nodiscard]] read_readiness poll_read() noexcept {
    return inbound().readable() ? read_readiness::ready : read_readiness::would_wait;
  }

  /** @brief Hands all of `data` to the peer. */
  [[nodiscard]] write_awaitable
  write_some(std::span<std::byte const> data QUOLL_LIFETIMEBOUND) noexcept QUOLL_LIFETIMEBOUND {
    return {outbound(), data};
  }

  /** @brief Stops sending, as close() does: see HalfClosableStream. */
  [[nodiscard]] shutdown_awaitable shutdown_send() noexcept QUOLL_LIFETIMEBOUND {
    return shutdown_awaitable{outbound()};
  }

  /**
   * @brief Closes the sending side: the peer reads what was already written, then `io::error::end_of_stream`.
   * Writing fails from now on; reading still works.
   */
  void close() { outbound().close(); }

private:
  friend std::pair<memory_stream, memory_stream> make_memory_stream_pair();

  memory_stream(std::shared_ptr<channels> pipe, std::size_t side) noexcept : channels_(std::move(pipe)), side_(side) {}

  // Each end reads from the channel of its side and writes into the other one.
  [[nodiscard]] detail::memory_channel &inbound() const noexcept {
    assert(channels_ && "memory_stream used after being moved from");
    return (*channels_)[side_];
  }

  [[nodiscard]] detail::memory_channel &outbound() const noexcept {
    assert(channels_ && "memory_stream used after being moved from");
    return (*channels_)[1 - side_];
  }

  // Closing wakes the peer's pending read through its executor. If that post throws (an executor whose host loop
  // allocates; run_loop's never does), the destructor and the move assignment, both noexcept, terminate: dropping
  // the wake-up instead would leave the peer suspended forever.
  void release() {
    if (channels_) {
      assert(!inbound().has_reader() && "memory_stream destroyed or moved while one of its reads is pending");
      outbound().close();
      inbound().detach_reader();
      channels_.reset();
    }
  }

  std::shared_ptr<channels> channels_;
  std::size_t side_;
};

static_assert(async::ContextAwaitable<memory_stream::read_awaitable>);
static_assert(async::ContextAwaitable<memory_stream::shutdown_awaitable>);
static_assert(async::ContextAwaitable<memory_stream::write_awaitable>);
static_assert(HalfClosableStream<memory_stream>);
static_assert(PollableStream<memory_stream>);

/** @brief Two connected memory_stream ends. */
[[nodiscard]] inline std::pair<memory_stream, memory_stream> make_memory_stream_pair() {
  auto pipe = std::make_shared<memory_stream::channels>();

  return {memory_stream{pipe, 0}, memory_stream{pipe, 1}};
}

} // namespace quoll::io

#endif // QUOLL_IO_MEMORY_STREAM_HPP
