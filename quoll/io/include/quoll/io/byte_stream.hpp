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

#ifndef QUOLL_IO_BYTE_STREAM_HPP
#define QUOLL_IO_BYTE_STREAM_HPP

#include <quoll/async/context_awaitable.hpp>
#include <quoll/io/io_result.hpp>

#include <concepts>
#include <cstddef>
#include <span>

namespace quoll::io {

/**
 * @brief A bidirectional stream of raw bytes (socket, TLS session, in-memory pipe).
 *
 * `read_some(buffer)` and `write_some(data)` return ContextAwaitables resuming with an io_result:
 * - `read_some` transfers at least one byte into `buffer`, unless `buffer` is empty or an error is reported;
 *   `io::error::end_of_stream` once the peer has closed its side and every byte sent before was read;
 * - `write_some` transfers at least one byte of `data`, unless `data` is empty or an error is reported.
 *
 * The bytes referenced by `buffer` / `data` must stay valid until the operation completes. At most one read and one
 * write are pending at a time.
 */
template <class S>
concept ByteStream = requires(S &stream, std::span<std::byte> buffer, std::span<std::byte const> data) {
  { stream.read_some(buffer) } -> async::ContextAwaitable;
  { stream.write_some(data) } -> async::ContextAwaitable;
  { stream.read_some(buffer).await_resume() } -> std::same_as<io_result>;
  { stream.write_some(data).await_resume() } -> std::same_as<io_result>;
};

} // namespace quoll::io

#endif // QUOLL_IO_BYTE_STREAM_HPP
