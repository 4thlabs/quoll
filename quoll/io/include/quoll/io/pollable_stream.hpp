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

#ifndef QUOLL_IO_POLLABLE_STREAM_HPP
#define QUOLL_IO_POLLABLE_STREAM_HPP

#include <quoll/io/byte_stream.hpp>

#include <concepts>
#include <cstdint>

namespace quoll::io {

/** @brief What a read would do if it started now, as poll_read() tells without reading. */
enum class read_readiness : std::uint8_t {
  /** @brief It would wait: nothing has come, and the peer has not closed the stream. */
  would_wait,
  /** @brief It would complete at once: with bytes, the end of the stream or an error. */
  ready,
  /** @brief The stream cannot tell without reading. */
  unknown,
};

/**
 * @brief A ByteStream that tells, without waiting nor taking anything, what a read started now would do.
 *
 * `poll_read()` reads nothing a read would then miss, and no read or write may be pending while it runs. A connection
 * kept idle between two requests is checked so before it is used again, as curl does: one the peer closed, or that
 * carries bytes nothing asked for, is not. The peer may still close it once poll_read() has answered.
 */
template <class S>
concept PollableStream = ByteStream<S> && requires(S &stream) {
  { stream.poll_read() } noexcept -> std::same_as<read_readiness>;
};

} // namespace quoll::io

#endif // QUOLL_IO_POLLABLE_STREAM_HPP
