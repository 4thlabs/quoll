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
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/detail/stream_vtable.hpp>
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
 * stream is kept with room for the awaitables of one read and one write. A stream that fits in 192 bytes with them,
 * and moves without throwing, is held in place; a bigger one is allocated once, when it is wrapped. Reading and
 * writing never allocate.
 *
 * Move-only. Like the wrapped stream, it must not be moved or destroyed while one of its operations is pending, and
 * has at most one read and one write pending at a time. A moved-from any_stream can only be assigned to or
 * destroyed.
 *
 * Thread safety: same as the wrapped stream.
 */
class any_stream {
public:
  /** @brief Awaitable returned by read_some() and write_some(). */
  class [[nodiscard]] awaitable {
  public:
    awaitable(awaitable const &) = delete;
    awaitable &operator=(awaitable const &) = delete;
    awaitable &operator=(awaitable &&) = delete;

    // The wrapped awaitable stays where it was built: only the handle on it moves.
    awaitable(awaitable &&other) noexcept
        : awaitable_(std::exchange(other.awaitable_, nullptr)), vtable_(other.vtable_), pending_(other.pending_) {}

    ~awaitable() {
      if (awaitable_ != nullptr) {
        vtable_->destroy(awaitable_);
        *pending_ = false;
      }
    }

    [[nodiscard]] bool await_ready() { return vtable_->ready(awaitable_); }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle,
                                                        async::await_context const &context) {
      return vtable_->suspend(awaitable_, handle, context);
    }

    [[nodiscard]] io_result await_resume() { return vtable_->resume(awaitable_); }

  private:
    friend class any_stream;

    awaitable(void *wrapped, detail::awaitable_vtable const &vtable, bool &pending) noexcept
        : awaitable_(wrapped), vtable_(&vtable), pending_(&pending) {}

    void *awaitable_;
    detail::awaitable_vtable const *vtable_;
    bool *pending_;
  };

  /** @brief Takes `stream` over. */
  // The exclusion comes first: it stops ByteStream<any_stream> from checking this constructor against itself.
  template <class S>
    requires(!std::same_as<std::remove_cvref_t<S>, any_stream> && !std::is_lvalue_reference_v<S> &&
             detail::erasable_stream<S> && std::move_constructible<S>)
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
  [[nodiscard]] awaitable read_some(std::span<std::byte> buffer) {
    assert(vtable_ != nullptr && "any_stream used after being moved from");
    assert(!reading_ && "any_stream: a read is already pending");

    auto *const wrapped = vtable_->read_some(block_, buffer);

    reading_ = true;
    return awaitable{wrapped, *vtable_->read, reading_};
  }

  /** @brief Writes with the wrapped stream. */
  [[nodiscard]] awaitable write_some(std::span<std::byte const> data) {
    assert(vtable_ != nullptr && "any_stream used after being moved from");
    assert(!writing_ && "any_stream: a write is already pending");

    auto *const wrapped = vtable_->write_some(block_, data);

    writing_ = true;
    return awaitable{wrapped, *vtable_->write, writing_};
  }

  /** @brief The wrapped stream if its type is exactly `S`, otherwise `nullptr`. */
  template <class S>
  [[nodiscard]] S *target() noexcept {
    return is<S>() ? &static_cast<detail::stream_block<S> *>(block_)->stream : nullptr;
  }

  /** @copydoc target */
  template <class S>
  [[nodiscard]] S const *target() const noexcept {
    return is<S>() ? &static_cast<detail::stream_block<S> const *>(block_)->stream : nullptr;
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

  void take(any_stream &other) noexcept {
    assert(!other.reading_ && !other.writing_ && "any_stream moved while one of its operations is pending");

    vtable_ = std::exchange(other.vtable_, nullptr);

    if (vtable_ == nullptr) {
      return;
    }

    if (vtable_->relocate != nullptr) {
      block_ = vtable_->relocate(other.block_, storage_);
    } else {
      block_ = other.block_;
    }

    other.block_ = nullptr;
  }

  void reset() noexcept {
    if (vtable_ != nullptr) {
      assert(!reading_ && !writing_ && "any_stream destroyed while one of its operations is pending");
      vtable_->destroy(block_);
      vtable_ = nullptr;
      block_ = nullptr;
    }
  }

  alignas(std::max_align_t) std::byte storage_[in_place_size];
  void *block_ = nullptr;
  detail::stream_vtable const *vtable_ = nullptr;
  bool reading_ = false;
  bool writing_ = false;
};

static_assert(async::ContextAwaitable<any_stream::awaitable>);
static_assert(ByteStream<any_stream>);

} // namespace quoll::io

#endif // QUOLL_IO_ANY_STREAM_HPP
