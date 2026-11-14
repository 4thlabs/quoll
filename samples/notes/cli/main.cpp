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
//   quoll_notes_cli [url]       (http://127.0.0.1:8080 by default)

#include "notes_api.hpp"

#include <quoll/async.hpp>
#include <quoll/rest.hpp>
#include <quoll/runtime.hpp>

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void print(std::vector<notes::note> const &found) {
  for (auto const &note : found) {
    std::cout << "  #" << note.id << ' ' << note.text << '\n';
  }
}

// The program: the client and its connection live in this coroutine, on the runtime.
quoll::async::task<> use_notes(std::string url) {
  // One connection, kept alive, carries the calls.
  quoll::rest::client<notes::api> client{std::move(url)};
  auto const milk = co_await client.call<notes::create>({.text = "buy milk"});

  co_await client.call<notes::create>({.text = "write the OpenAPI generator"});
  co_await client.call<notes::create>({.text = "buy coffee"});
  std::cout << "created #" << milk.id << ", all the notes:\n";
  print(co_await client.call<notes::list>({}));

  std::cout << "the notes about buying:\n";
  print(co_await client.call<notes::list>({.contains = "buy"}));

  co_await client.call<notes::remove>(milk.id);
  std::cout << "removed #" << milk.id << '\n';

  try {
    co_await client.call<notes::get>(milk.id);
  } catch (quoll::http::http_error const &error) {
    // The detail of the server's problem.
    std::cout << "get #" << milk.id << ": " << static_cast<int>(error.status()) << ' ' << error.what() << '\n';
  }
}

} // namespace

int main(int argc, char **argv) {
  quoll::runtime runtime;

  return runtime.run(use_notes(argc > 1 ? argv[1] : "http://127.0.0.1:8080"));
}
