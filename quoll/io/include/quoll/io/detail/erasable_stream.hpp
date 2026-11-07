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

#ifndef QUOLL_IO_DETAIL_ERASABLE_STREAM_HPP
#define QUOLL_IO_DETAIL_ERASABLE_STREAM_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>

#include <coroutine>
#include <cstddef>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

namespace quoll::io::detail {

template <class S>
using read_awaitable_t = decltype(std::declval<S &>().read_some(std::declval<std::span<std::byte>>()));

template <class S>
using write_awaitable_t = decltype(std::declval<S &>().write_some(std::declval<std::span<std::byte const>>()));

// What shutting down a stream that cannot is: an error, at once.
struct shutdown_not_supported {
  [[nodiscard]] bool await_ready() const noexcept { return true; }

  [[nodiscard]] bool await_suspend(std::coroutine_handle<>, async::await_context const &) const noexcept {
    return false;
  }

  [[nodiscard]] io_result await_resume() const noexcept {
    return {std::make_error_code(std::errc::operation_not_supported), 0};
  }
};

template <class S>
struct shutdown_awaitable {
  using type = shutdown_not_supported;
};

template <HalfClosableStream S>
struct shutdown_awaitable<S> {
  using type = decltype(std::declval<S &>().shutdown_send());
};

template <class S>
using shutdown_awaitable_t = typename shutdown_awaitable<S>::type;

// A stream whose operations return their awaitable by value: it can be built in a slot of the block.
template <class S>
concept ErasableStream = ByteStream<S> && std::is_object_v<read_awaitable_t<S>> &&
                         std::is_object_v<write_awaitable_t<S>> && std::is_object_v<shutdown_awaitable_t<S>>;

} // namespace quoll::io::detail

#endif // QUOLL_IO_DETAIL_ERASABLE_STREAM_HPP
