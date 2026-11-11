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

// Serves the notes API over HTTP until Ctrl-C:
//
//   quoll_notes_server [port]      (8080 by default, 0 for any free port)
//   curl -X POST localhost:8080/api/notes -H 'Content-Type: application/json' -d '{"text":"buy milk"}'
//   curl 'localhost:8080/api/notes?contains=milk'

#include "notes_api.hpp"
#include "service.hpp"

#include <quoll/async.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/rest.hpp>
#include <quoll/runtime.hpp>
#include <quoll/server.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace {

// The program: the server lives in this coroutine, on the runtime. Listening first, then serving
// (what quoll::http::server::listen does in one call), tells when the server is ready: once the address is printed.
quoll::async::task<> serve_notes(std::uint16_t port) {
  quoll::http::server server;

  quoll::rest::mount<notes::api>(server, notes::service{});

  auto listener = co_await quoll::io::listen(port);

  // With port 0, the system picks a free one.
  std::cout << "notes API on http://localhost:" << listener.local_port().value_or(port) << "/api/notes" << std::endl;
  co_await server.serve(std::move(listener));
  std::cout << "stopped\n";
}

} // namespace

int main(int argc, char **argv) {
  auto const port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8080);
  quoll::runtime runtime;

  // Ctrl-C stops the token, which ends the server.
  return runtime.run(serve_notes(port));
}
