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

// Calls the notes API served by quoll_notes_server:
//
//   quoll_notes_client [host] [port]

#include "notes_api.hpp"

#include <quoll/asio.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/task.hpp>
#include <quoll/client.hpp>
#include <quoll/rest.hpp>

#include <asio/io_context.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace endpoints = notes::endpoints;

using notes_client = quoll::rest::client<notes::api, quoll::client::transport<quoll::asio::connector>>;

void print(std::vector<notes::note> const &found) {
  for (auto const &note : found) {
    std::cout << "  #" << note.id << ' ' << note.text << '\n';
  }
}

quoll::async::task<> use_notes(notes_client client) {
  auto const milk = co_await client.call<endpoints::create>({.text = "buy milk"});

  (void)co_await client.call<endpoints::create>({.text = "write the OpenAPI generator"});
  (void)co_await client.call<endpoints::create>({.text = "buy coffee"});
  std::cout << "created #" << milk.id << ", all the notes:\n";
  print(co_await client.call<endpoints::list>({}));

  std::cout << "the notes about buying:\n";
  print(co_await client.call<endpoints::list>({.contains = "buy"}));

  co_await client.call<endpoints::remove>(milk.id);
  std::cout << "removed #" << milk.id << '\n';

  try {
    (void)co_await client.call<endpoints::get>(milk.id);
  } catch (quoll::rest::call_error const &error) {
    // The detail of the server's problem.
    std::cout << "get #" << milk.id << ": " << static_cast<int>(error.status()) << ' ' << error.what() << '\n';
  }
}

// What becomes of the calls.
struct report_end {
  int &exit_code;

  void operator()() const { exit_code = 0; }

  void operator()(std::exception_ptr failure) const {
    try {
      std::rethrow_exception(std::move(failure));
    } catch (std::exception const &error) {
      std::cerr << "client failed: " << error.what() << '\n';
    }
  }
};

} // namespace

int main(int argc, char **argv) {
  std::string const host = argc > 1 ? argv[1] : "127.0.0.1";
  auto const port = static_cast<std::uint16_t>(argc > 2 ? std::stoi(argv[2]) : 8080);
  ::asio::io_context context;
  // The transport keeps its connection alive: every call below goes over one connection.
  notes_client client{quoll::client::transport{quoll::asio::connector{context.get_executor()}, host, port}};
  int exit_code = 1;

  co_spawn(quoll::asio::executor{context.get_executor()}, use_notes(std::move(client)), report_end{exit_code});
  context.run();
  return exit_code;
}
