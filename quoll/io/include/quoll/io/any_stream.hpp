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

#ifndef QUOLL_IO_ANY_STREAM_HPP
#define QUOLL_IO_ANY_STREAM_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/detail/erasable_stream.hpp>
#include <quoll/io/detail/sound_read.hpp>
#include <quoll/io/detail/stream_vtable.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace quoll::io {

/**
 * @brief Owning, type-erased ByteStream; models ByteStream itself.
 *
 * Wraps any ByteStream whose operations return their awaitable by value (a socket, a TLS session, a memory_stream)
 * behind one type, for code that cannot be a template over the stream: connectors, connection pools. The wrapped
 * stream is kept with room for the awaitables of one read, one write and one shutdown. A stream that fits in 192 bytes
 * with them, and moves without throwing, is held in place; a bigger one is allocated once, when it is wrapped. Reading
 * and writing never allocate.
 *
 * Move-only. Like the wrapped stream, it must not be moved or destroyed while one of its operations is pending, and
 * has at most one read and one write pending at a time: an operation is pending from its call until it has completed,
 * or until its awaitable goes, and starting another then throws std::logic_error, as does awaiting one twice. A
 * moved-from any_stream can only be assigned to or destroyed: an operation on it throws the same. Moving or destroying
 * one with an operation pending terminates, as it has nothing to throw to and the awaitable would go on using it.
 *
 * Thread safety: same as the wrapped stream.
 */
class any_stream {
public:
  /** @brief Awaitable returned by read_some(), write_some() and shutdown_send(). */
  class [[nodiscard]] awaitable {
  public:
    awaitable(awaitable const &) = delete;
    awaitable &operator=(awaitable const &) = delete;
    awaitable &operator=(awaitable &&) = delete;

    // The wrapped awaitable stays where it was built: only the handle on it moves.
    awaitable(awaitable &&other) noexcept
        : awaitable_(std::exchange(other.awaitable_, nullptr)), vtable_(other.vtable_), pending_(other.pending_),
          asked_(other.asked_) {}

    ~awaitable() { finish(); }

    [[nodiscard]] bool await_ready() {
      if (awaitable_ == nullptr) {
        throw_finished();
      }

      return vtable_->ready(awaitable_);
    }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle,
                                                        async::await_context const &context) {
      return vtable_->suspend(awaitable_, handle, context);
    }

    // The next operation may start as soon as this one has its result, whatever holds this object, and its slot is
    // free by then. A read reports what can be trusted, for every reader at once: see detail::sound_read.
    [[nodiscard]] io_result await_resume() {
      finish_on_exit const finished{*this};
      auto const result = vtable_->resume(awaitable_);

      return asked_ == not_a_read ? result : detail::sound_read(result, asked_);
    }

  private:
    friend class any_stream;

    struct finish_on_exit {
      awaitable &operation;

      explicit finish_on_exit(awaitable &finishing) noexcept : operation(finishing) {}

      finish_on_exit(finish_on_exit const &) = delete;
      finish_on_exit &operator=(finish_on_exit const &) = delete;

      ~finish_on_exit() { operation.finish(); }
    };

    void finish() noexcept {
      if (awaitable_ != nullptr) {
        vtable_->destroy(awaitable_);
        awaitable_ = nullptr;
        *pending_ = false;
      }
    }

    static constexpr std::size_t not_a_read = static_cast<std::size_t>(-1);

    awaitable(void *wrapped, detail::awaitable_vtable const &vtable, bool &pending,
              std::size_t asked = not_a_read) noexcept
        : awaitable_(wrapped), vtable_(&vtable), pending_(&pending), asked_(asked) {}

    void *awaitable_;
    detail::awaitable_vtable const *vtable_;
    bool *pending_;
    std::size_t asked_;
  };

  /** @brief Takes `stream` over. */
  // The exclusion comes first: it stops ByteStream<any_stream> from checking this constructor against itself.
  template <class S>
    requires(!std::same_as<std::remove_cvref_t<S>, any_stream> && !std::is_lvalue_reference_v<S> &&
             detail::ErasableStream<S> && std::move_constructible<S>)
  explicit any_stream(S &&stream) {
    using block = detail::stream_block<S>;

    if constexpr (fits_in_place<S>()) {
      block_ = std::construct_at(reinterpret_cast<block *>(storage_), std::move(stream));
      vtable_ = &detail::stream_vtable_for<S, true>;
    } else {
      block_ = std::make_unique<block>(std::move(stream)).release();
      vtable_ = &detail::stream_vtable_for<S, false>;
    }
  }

  any_stream(any_stream const &) = delete;
  any_stream &operator=(any_stream const &) = delete;

  any_stream(any_stream &&other) noexcept { take(other); }

  any_stream &operator=(any_stream &&other) noexcept {
    if (this != &other) {
      reset();
      take(other);
    }

    return *this;
  }

  ~any_stream() { reset(); }

  /** @brief Reads with the wrapped stream. */
  [[nodiscard]] awaitable read_some(std::span<std::byte> buffer QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    refuse_if(vtable_ == nullptr || reading_ || shutting_down_);

    auto *const wrapped = vtable_->read_some(block_, buffer);

    reading_ = true;
    return awaitable{wrapped, *vtable_->read, reading_, buffer.size()};
  }

  /** @brief Writes with the wrapped stream. */
  [[nodiscard]] awaitable write_some(std::span<std::byte const> data QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    refuse_if(vtable_ == nullptr || writing_ || shutting_down_);

    auto *const wrapped = vtable_->write_some(block_, data);

    writing_ = true;
    return awaitable{wrapped, *vtable_->write, writing_};
  }

  /**
   * @brief Stops sending, with the wrapped stream: see HalfClosableStream. A stream that is not one reports
   * `std::errc::operation_not_supported`, so that a caller goes on as it would with no way to do this.
   */
  [[nodiscard]] awaitable shutdown_send() QUOLL_LIFETIMEBOUND {
    refuse_if(vtable_ == nullptr || reading_ || writing_ || shutting_down_);

    auto *const wrapped = vtable_->shutdown_send(block_);

    shutting_down_ = true;
    return awaitable{wrapped, *vtable_->shutdown, shutting_down_};
  }

  /** @brief The wrapped stream if its type is exactly `S`, otherwise `nullptr`. */
  template <class S>
  [[nodiscard]] S *target() noexcept QUOLL_LIFETIMEBOUND {
    if constexpr (ByteStream<S>) {
      return is<S>() ? &static_cast<detail::stream_block<S> *>(block_)->stream : nullptr;
    } else {
      return nullptr;
    }
  }

  /** @copydoc target */
  template <class S>
  [[nodiscard]] S const *target() const noexcept QUOLL_LIFETIMEBOUND {
    if constexpr (ByteStream<S>) {
      return is<S>() ? &static_cast<detail::stream_block<S> const *>(block_)->stream : nullptr;
    } else {
      return nullptr;
    }
  }

private:
  static constexpr std::size_t in_place_size = 192;

  template <class S>
  [[nodiscard]] static consteval bool fits_in_place() noexcept {
    using block = detail::stream_block<S>;

    return sizeof(block) <= in_place_size && alignof(block) <= alignof(std::max_align_t) &&
           std::is_nothrow_move_constructible_v<S>;
  }

  template <class S>
  [[nodiscard]] bool is() const noexcept {
    return vtable_ != nullptr && vtable_->type == async::detail::type_id_of<S>();
  }

  // An operation over a live one would build over its awaitable in the slot, and one on a moved-from stream has no
  // table to go through, undefined in a build with no assert: not worth the branch it saves.
  static void refuse_if(bool refused) {
    if (refused) {
      throw_refused();
    }
  }

  [[noreturn]] static void throw_refused();
  [[noreturn]] static void throw_finished();

  void take(any_stream &other) noexcept;
  void reset() noexcept;

  alignas(std::max_align_t) std::byte storage_[in_place_size];
  void *block_ = nullptr;
  detail::stream_vtable const *vtable_ = nullptr;
  bool reading_ = false;
  bool writing_ = false;
  bool shutting_down_ = false;
};

static_assert(async::ContextAwaitable<any_stream::awaitable>);
static_assert(ByteStream<any_stream>);
static_assert(HalfClosableStream<any_stream>);

} // namespace quoll::io

#endif // QUOLL_IO_ANY_STREAM_HPP
