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

#ifndef QUOLL_TEST_MEMORY_NETWORK_HPP
#define QUOLL_TEST_MEMORY_NETWORK_HPP

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/overloaded.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <utility>

namespace quoll::test {

// The server end of the connections opened by a memory_connector: each one is served on the loop by `serve`.
struct memory_server {
  async::run_loop::executor executor;
  std::function<async::task<>(io::memory_stream)> serve;
  int connections = 0;
  // Where the last connection went, as `host:port`.
  std::string destination;
  std::exception_ptr failure;
};

// A connector whose connections are memory streams, served by a memory_server: an http::client over it reaches an
// http::server of the same test, with no network.
class memory_connector {
public:
  explicit memory_connector(memory_server &server) noexcept : server_(&server) {}

  [[nodiscard]] async::task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    auto *const server = server_;
    auto [client_end, server_end] = io::make_memory_stream_pair();

    ++server->connections;
    server->destination = host + ':' + std::to_string(port);
    co_spawn(
        server->executor, server->serve(std::move(server_end)),
        overloaded{
            [] {}, [server](std::exception_ptr error) { server->failure = std::move(error); }, [](async::stopped_t) {}
        }
    );
    co_return io::any_stream{std::move(client_end)};
  }

private:
  memory_server *server_;
};

static_assert(io::Connector<memory_connector>);

// What serves each connection with `router`, which outlives the server.
[[nodiscard]] inline std::function<async::task<>(io::memory_stream)> serving(http::router const &router) {
  return [&router](io::memory_stream stream) { return http::server{router}.serve_connection(std::move(stream)); };
}

} // namespace quoll::test

#endif // QUOLL_TEST_MEMORY_NETWORK_HPP
