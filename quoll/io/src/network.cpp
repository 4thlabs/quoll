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

#include <quoll/io/connect.hpp>
#include <quoll/io/connect_tls.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/io/listen_tls.hpp>

#include <quoll/async/current_executor.hpp>
#include <quoll/async/detail/require_service.hpp>
#include <quoll/io/network_ref.hpp>
#include <quoll/io/tls_network_ref.hpp>

#include <stdexcept>
#include <utility>

namespace quoll::io {

namespace {

// How async::detail::require_service() names the service, for an application that finds it missing: often, OpenSSL was
// not there when the build was configured.
constexpr char const *tls_service_name = "TLS network, which needs OpenSSL when Quoll is configured";

} // namespace

async::task<any_stream> connect(std::string host, std::uint16_t port) {
  auto const network =
      async::detail::require_service<network_ref>(co_await async::current_executor(), "quoll::io::connect", "network");

  co_return co_await network.connect(std::move(host), port);
}

async::task<any_stream> connect_tls(std::string host, std::uint16_t port, tls_client_options options) {
  auto const network = async::detail::require_service<tls_network_ref>(co_await async::current_executor(),
                                                                       "quoll::io::connect_tls", tls_service_name);

  co_return co_await network.connect_tls(std::move(host), port, options);
}

async::task<any_listener> listen(std::uint16_t port, std::string address) {
  auto const network =
      async::detail::require_service<network_ref>(co_await async::current_executor(), "quoll::io::listen", "network");

  co_return network.listen(std::move(address), port);
}

async::task<any_listener> listen_tls(std::uint16_t port, tls_server_options options, std::string address) {
  if (options.certificate_file.empty() || options.private_key_file.empty()) {
    throw std::invalid_argument{"quoll::io::listen_tls: both a certificate and its private key are needed"};
  }

  auto const network = async::detail::require_service<tls_network_ref>(co_await async::current_executor(),
                                                                       "quoll::io::listen_tls", tls_service_name);

  co_return network.listen_tls(std::move(address), port, options);
}

} // namespace quoll::io
