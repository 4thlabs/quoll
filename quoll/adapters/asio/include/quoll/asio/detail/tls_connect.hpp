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

#ifndef QUOLL_ASIO_DETAIL_TLS_CONNECT_HPP
#define QUOLL_ASIO_DETAIL_TLS_CONNECT_HPP

#if defined(QUOLL_ASIO_TLS)

#include <quoll/asio/detail/connect.hpp>
#include <quoll/asio/tls_stream.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/tls_options.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/ip/address.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/ssl/verify_mode.hpp>

#include <openssl/ssl.h>

#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::asio::detail {

// TLS 1.2 at least: the versions before are broken.
inline void require_modern_tls(::asio::ssl::context &context) {
  if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) != 1) {
    throw std::system_error{
        std::make_error_code(std::errc::protocol_not_supported), "quoll::asio: cannot require TLS 1.2"
    };
  }
}

[[nodiscard]] inline ::asio::ssl::context client_context(io::tls_client_options const &options) {
  ::asio::ssl::context context{::asio::ssl::context::tls_client};

  require_modern_tls(context);

  if (!options.verify_peer) {
    context.set_verify_mode(::asio::ssl::verify_none);
    return context;
  }

  context.set_verify_mode(::asio::ssl::verify_peer);

  if (options.ca_file.empty()) {
    context.set_default_verify_paths();
  } else {
    context.load_verify_file(options.ca_file);
  }

  return context;
}

// The TCP connection, then the session on it, under the awaiter's stop token all along. A context per connection: what
// it holds is loaded in a moment next to the handshake, and connections are kept once opened.
inline async::task<io::any_stream>
connect_tls(::asio::any_io_executor executor, std::string host, std::uint16_t port, io::tls_client_options options) {
  auto context = client_context(options);
  tls_stream stream{tls_stream::session_type{co_await connect_socket(std::move(executor), host, port), context}};
  auto &session = stream.session();

  if (options.verify_peer) {
    session.set_verify_callback(::asio::ssl::host_name_verification{host});
  }

  std::error_code not_an_address;

  // The name of the server, for one serving several (SNI). Not for an address: TLS forbids it.
  static_cast<void>(::asio::ip::make_address(host, not_an_address));

  if (not_an_address && SSL_set_tlsext_host_name(session.native_handle(), host.c_str()) != 1) {
    throw std::system_error{
        std::make_error_code(std::errc::invalid_argument), "quoll::asio: cannot send the server's name: " + host
    };
  }

  if (auto const error = co_await stream.handshake(tls_stream::session_type::client)) {
    throw std::system_error{error, "quoll::asio: TLS handshake with " + host + " failed"};
  }

  co_return io::any_stream{std::move(stream)};
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_TLS

#endif // QUOLL_ASIO_DETAIL_TLS_CONNECT_HPP
