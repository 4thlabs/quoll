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

#ifndef QUOLL_CLIENT_DETAIL_CLIENT_BACKEND_HPP
#define QUOLL_CLIENT_DETAIL_CLIENT_BACKEND_HPP

#include <quoll/async/task.hpp>
#include <quoll/client/client_options.hpp>
#include <quoll/client/detail/connector_handle.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/streamed_response.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace quoll::http::detail {

// What a client does, compiled once: over a connector, or through the platform of the executor.
struct client_operations {
  async::task<http::response> (*send)(std::shared_ptr<void> const &state, http::request request);
  async::task<http::streamed_response> (*stream)(std::shared_ptr<void> const &state, http::request request);
};

struct client_backend {
  std::shared_ptr<void> state;
  client_operations const *operations;
};

[[nodiscard]] client_backend make_client_backend(
    connector_handle connector, std::string host, std::uint16_t port, bool secure, std::string prefix,
    client_options const &options
);

// A client from `url` through the executor's http::platform_ref: what a client from a URL is in a build for Emscripten.
// Compiled in every build, in a source of its own, so that it runs in the native sanitizers' tests too.
[[nodiscard]] client_backend make_platform_backend(std::string_view url, client_options const &options);

} // namespace quoll::http::detail

#endif // QUOLL_CLIENT_DETAIL_CLIENT_BACKEND_HPP
