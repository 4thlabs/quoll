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

#ifndef QUOLL_CLIENT_DETAIL_CONNECTOR_HANDLE_HPP
#define QUOLL_CLIENT_DETAIL_CONNECTOR_HANDLE_HPP

#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/tls_connector.hpp>
#include <quoll/io/tls_options.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace quoll::http::detail {

// A connector, whichever it is: the core owns it through this.
struct connector_handle {
  std::shared_ptr<void const> connector;
  async::task<io::any_stream> (*open)(void const *connector, std::string const &host, std::uint16_t port, bool secure,
                                      io::tls_client_options const &tls);
};

// `connector`, erased: shared, as one may move without copying, and opened with TLS when asked, if it can.
template <io::Connector Connector>
[[nodiscard]] connector_handle make_connector_handle(Connector connector) {
  return {std::make_shared<Connector const>(std::move(connector)),
          [](void const *erased, std::string const &to, std::uint16_t on, [[maybe_unused]] bool use_tls,
             [[maybe_unused]] io::tls_client_options const &tls) -> async::task<io::any_stream> {
            auto const &target = *static_cast<Connector const *>(erased);

            if constexpr (io::TlsConnector<Connector>) {
              if (use_tls) {
                return target.connect_tls(to, on, tls);
              }
            }

            return target.connect(to, on);
          }};
}

} // namespace quoll::http::detail

#endif // QUOLL_CLIENT_DETAIL_CONNECTOR_HANDLE_HPP
