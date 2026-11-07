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

#ifndef QUOLL_IO_HALF_CLOSABLE_STREAM_HPP
#define QUOLL_IO_HALF_CLOSABLE_STREAM_HPP

#include <quoll/async/context_awaitable.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/io_result.hpp>

#include <concepts>

namespace quoll::io {

/**
 * @brief A ByteStream that can stop sending and go on reading.
 *
 * `shutdown_send()` returns a ContextAwaitable resuming with an io_result, which any_stream makes
 * `std::errc::operation_not_supported` over a stream that is not one: the peer then reads what was sent, and
 * `io::error::end_of_stream`. Reading still works, and nothing may be written afterwards. No read or write may be
 * pending while it runs.
 *
 * Closing only this side lets a peer that is still sending finish and read what it was sent. Closing a socket with
 * bytes of the peer unread resets the connection, which can destroy a response on its way.
 */
template <class S>
concept HalfClosableStream = ByteStream<S> && requires(S &stream) {
  { stream.shutdown_send() } -> async::ContextAwaitable;
  { stream.shutdown_send().await_resume() } -> std::same_as<io_result>;
};

} // namespace quoll::io

#endif // QUOLL_IO_HALF_CLOSABLE_STREAM_HPP
