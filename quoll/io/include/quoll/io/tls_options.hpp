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

#ifndef QUOLL_IO_TLS_OPTIONS_HPP
#define QUOLL_IO_TLS_OPTIONS_HPP

#include <string>

namespace quoll::io {

/** @brief How a client checks the servers it connects to over TLS. */
struct tls_client_options {
  /** @brief A PEM file of the authorities to trust, instead of the system's: for a private authority. */
  std::string ca_file{};

  /**
   * @brief Checks the server's certificate and that it names the host. Turning it off accepts anyone claiming to be the
   * server: for a development setup only.
   */
  bool verify_peer = true;
};

/** @brief What a server presents over TLS. */
struct tls_server_options {
  /** @brief A PEM file: the server's certificate, then its chain. */
  std::string certificate_file{};

  /** @brief A PEM file: the private key of the certificate. */
  std::string private_key_file{};

  /** @brief Whether any of it is set: without, a server speaks plain HTTP. */
  [[nodiscard]] bool enabled() const noexcept { return !certificate_file.empty() || !private_key_file.empty(); }
};

} // namespace quoll::io

#endif // QUOLL_IO_TLS_OPTIONS_HPP
