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

#ifndef QUOLL_IO_WRITE_HPP
#define QUOLL_IO_WRITE_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/io_result.hpp>

#include <cstddef>
#include <span>
#include <system_error>

namespace quoll::io {

/**
 * @brief Writes all of `data`, with as many `write_some` as it takes. On error, `transferred` counts the bytes
 * written before it. A stream that reports no error and no progress, or more than it was given, is such an error:
 * `std::errc::io_error`. `stream` and `data` must outlive the returned task.
 */
template <ByteStream Stream>
[[nodiscard]] async::task<io_result> write(Stream &stream QUOLL_LIFETIMEBOUND,
                                           std::span<std::byte const> data QUOLL_LIFETIMEBOUND) {
  std::size_t written = 0;

  while (written < data.size()) {
    auto const result = co_await stream.write_some(data.subspan(written));

    if (result.transferred > data.size() - written) {
      co_return io_result{result.error ? result.error : std::make_error_code(std::errc::io_error), written};
    }

    written += result.transferred;

    if (result.error) {
      co_return io_result{result.error, written};
    }

    // ByteStream contract: without an error, a write of non-empty data makes progress. One that breaks it would have
    // this loop spin for ever, with nothing to stop it if the stream never suspends.
    if (result.transferred == 0) {
      co_return io_result{std::make_error_code(std::errc::io_error), written};
    }
  }

  co_return io_result{{}, written};
}

} // namespace quoll::io

#endif // QUOLL_IO_WRITE_HPP
