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

#ifndef QUOLL_ASIO_DETAIL_IO_ERROR_HPP
#define QUOLL_ASIO_DETAIL_IO_ERROR_HPP

#include <quoll/io/error.hpp>

#include <asio/error.hpp>

#if defined(QUOLL_ASIO_TLS)
#include <asio/ssl/error.hpp>
#endif

#include <system_error>

namespace quoll::asio::detail {

// The error codes of the ByteStream contract for the asio ones that have a counterpart.
[[nodiscard]] inline std::error_code to_io_error(std::error_code error) noexcept {
  if (error == ::asio::error::eof) {
    return io::error::end_of_stream;
  }

  if (error == ::asio::error::operation_aborted) {
    return std::make_error_code(std::errc::operation_canceled);
  }

#if defined(QUOLL_ASIO_TLS)
  // The connection ended without TLS's own closing message: the data may have been cut by anyone, so it is no regular
  // end of the stream.
  if (error == ::asio::ssl::error::stream_truncated) {
    return std::make_error_code(std::errc::connection_reset);
  }
#endif

  return error;
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_IO_ERROR_HPP
