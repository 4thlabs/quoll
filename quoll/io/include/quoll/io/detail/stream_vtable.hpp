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

#ifndef QUOLL_IO_DETAIL_STREAM_VTABLE_HPP
#define QUOLL_IO_DETAIL_STREAM_VTABLE_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/detail/awaiting.hpp>
#include <quoll/async/detail/type_id.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/detail/erasable_stream.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/pollable_stream.hpp>

#include <coroutine>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace quoll::io::detail {

// An erased stream and room for the awaitables of its pending read, write and shutdown: allocated once with the stream,
// so that operations never allocate.
template <ByteStream S>
struct stream_block {
  explicit stream_block(S &&wrapped) noexcept(std::is_nothrow_move_constructible_v<S>) : stream(std::move(wrapped)) {}

  stream_block(stream_block const &) = delete;
  stream_block &operator=(stream_block const &) = delete;
  stream_block &operator=(stream_block &&) = delete;

  // Only moved, to relocate a block held in place, while no operation is pending: the slots hold nothing.
  stream_block(stream_block &&other) noexcept
    requires std::is_nothrow_move_constructible_v<S>
      : stream(std::move(other.stream)) {}

  ~stream_block() = default;

  S stream;
  alignas(read_awaitable_t<S>) std::byte read_slot[sizeof(read_awaitable_t<S>)];
  alignas(write_awaitable_t<S>) std::byte write_slot[sizeof(write_awaitable_t<S>)];
  alignas(shutdown_awaitable_t<S>) std::byte shutdown_slot[sizeof(shutdown_awaitable_t<S>)];
};

// The operations of an awaitable built in a slot. await_suspend is normalized to the coroutine to resume: the
// awaiter itself when the awaitable declines to suspend, noop_coroutine() when nothing is to be resumed.
struct awaitable_vtable {
  bool (*ready)(void *awaitable);
  std::coroutine_handle<> (*suspend)(void *awaitable, std::coroutine_handle<> handle,
                                     async::await_context const &context);
  io_result (*resume)(void *awaitable);
  void (*destroy)(void *awaitable) noexcept;
};

// Through Quoll's own awaiting, as a co_await is: a stream's awaitable may be a task, whose await_suspend only that
// reaches.
template <class A>
[[nodiscard]] std::coroutine_handle<> suspend(A &awaitable, std::coroutine_handle<> handle,
                                              async::await_context const &context) {
  using async::detail::awaiting;
  using result = decltype(awaiting::suspend(awaitable, handle, context));

  if constexpr (std::is_void_v<result>) {
    awaiting::suspend(awaitable, handle, context);
    return std::noop_coroutine();
  } else if constexpr (std::is_same_v<result, bool>) {
    return awaiting::suspend(awaitable, handle, context) ? std::noop_coroutine() : handle;
  } else {
    return awaiting::suspend(awaitable, handle, context);
  }
}

template <class A>
inline constexpr awaitable_vtable awaitable_vtable_for{
    [](void *awaitable) -> bool { return static_cast<A *>(awaitable)->await_ready(); },
    [](void *awaitable, std::coroutine_handle<> handle, async::await_context const &context) {
      return detail::suspend(*static_cast<A *>(awaitable), handle, context);
    },
    [](void *awaitable) -> io_result { return static_cast<A *>(awaitable)->await_resume(); },
    [](void *awaitable) noexcept { std::destroy_at(static_cast<A *>(awaitable)); },
};

using relocate_function = void *(*)(void *block, void *target) noexcept;

struct stream_vtable {
  // Build the awaitable of an operation in its slot of the block, and return it.
  void *(*read_some)(void *block, std::span<std::byte> buffer);
  void *(*write_some)(void *block, std::span<std::byte const> data);
  void *(*shutdown_send)(void *block);
  // Null for a stream that is not a PollableStream.
  read_readiness (*poll_read)(void *block) noexcept;
  awaitable_vtable const *read;
  awaitable_vtable const *write;
  awaitable_vtable const *shutdown;
  void (*destroy)(void *block) noexcept;
  // Moves a block held in place into the storage at `target`, destroys the original and returns the new block;
  // null for a block on the heap, whose pointer is moved instead.
  relocate_function relocate;
  async::detail::type_id type;
};

template <ByteStream S, bool InPlace>
[[nodiscard]] consteval relocate_function relocate_for() noexcept {
  if constexpr (InPlace) {
    return [](void *block, void *target) noexcept -> void * {
      auto *const source = static_cast<stream_block<S> *>(block);
      auto *const moved = std::construct_at(static_cast<stream_block<S> *>(target), std::move(*source));

      std::destroy_at(source);
      return moved;
    };
  } else {
    return nullptr;
  }
}

template <ByteStream S>
[[nodiscard]] consteval auto poll_read_for() noexcept -> read_readiness (*)(void *) noexcept {
  if constexpr (PollableStream<S>) {
    return [](void *block) noexcept { return static_cast<stream_block<S> *>(block)->stream.poll_read(); };
  } else {
    return nullptr;
  }
}

template <ByteStream S, bool InPlace>
inline constexpr stream_vtable stream_vtable_for{
    // Placement new rather than construct_at: the awaitable returned by the stream is built in the slot, never
    // moved, so awaitables that cannot move are supported.
    [](void *block, std::span<std::byte> buffer) -> void * {
      auto &self = *static_cast<stream_block<S> *>(block);

      return ::new (static_cast<void *>(self.read_slot)) read_awaitable_t<S>(self.stream.read_some(buffer));
    },
    [](void *block, std::span<std::byte const> data) -> void * {
      auto &self = *static_cast<stream_block<S> *>(block);

      return ::new (static_cast<void *>(self.write_slot)) write_awaitable_t<S>(self.stream.write_some(data));
    },
    [](void *block) -> void * {
      auto &self = *static_cast<stream_block<S> *>(block);

      if constexpr (HalfClosableStream<S>) {
        return ::new (static_cast<void *>(self.shutdown_slot)) shutdown_awaitable_t<S>(self.stream.shutdown_send());
      } else {
        return ::new (static_cast<void *>(self.shutdown_slot)) shutdown_not_supported{};
      }
    },
    poll_read_for<S>(),
    &awaitable_vtable_for<read_awaitable_t<S>>,
    &awaitable_vtable_for<write_awaitable_t<S>>,
    &awaitable_vtable_for<shutdown_awaitable_t<S>>,
    [](void *block) noexcept {
      if constexpr (InPlace) {
        std::destroy_at(static_cast<stream_block<S> *>(block));
      } else {
        std::default_delete<stream_block<S>>{}(static_cast<stream_block<S> *>(block));
      }
    },
    relocate_for<S, InPlace>(),
    async::detail::type_id_of<S>(),
};

} // namespace quoll::io::detail

#endif // QUOLL_IO_DETAIL_STREAM_VTABLE_HPP
