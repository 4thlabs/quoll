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

#include <quoll/async.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/rest.hpp>
#include <quoll/runtime.hpp>
#include <quoll/server.hpp>

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

// The notes, in memory. The runtime runs the requests on one thread: no lock.
class notes_service {
public:
  std::vector<notes::note> operator()(notes::list, notes::note_filter filter) const {
    std::vector<notes::note> found;

    for (auto const &[id, note] : notes_) {
      if (!filter.contains || note.text.contains(*filter.contains)) {
        found.push_back(note);
      }
    }

    return found;
  }

  notes::note operator()(notes::get, std::int64_t id) const {
    auto const found = notes_.find(id);

    if (found == notes_.end()) {
      // An error status the endpoint does not declare: answered as an RFC 9457 problem.
      throw quoll::rest::http_error{quoll::http::status::not_found, "no note " + std::to_string(id)};
    }

    return found->second;
  }

  notes::note operator()(notes::create, notes::new_note created) {
    notes::note note{.id = next_id_++, .text = std::move(created.text)};

    notes_.emplace(note.id, note);
    return note;
  }

  void operator()(notes::remove, std::int64_t id) { notes_.erase(id); }

private:
  std::map<std::int64_t, notes::note> notes_;
  std::int64_t next_id_ = 1;
};

// The program: the router and its server live in this coroutine, on the runtime. Listening first, then serving
// (what quoll::http::server::listen does in one call), tells when the server is ready: once the address is printed.
quoll::async::task<> serve_notes(std::uint16_t port) {
  quoll::http::router router;

  quoll::rest::mount<notes::api>(router, notes_service{});

  auto listener = co_await quoll::io::listen(port);

  // With port 0, the system picks a free one.
  std::cout << "notes API on http://localhost:" << listener.local_port().value_or(port) << "/api/notes" << std::endl;
  co_await quoll::http::server{router}.serve(listener);
  std::cout << "stopped\n";
}

} // namespace

int main(int argc, char **argv) {
  auto const port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8080);
  quoll::runtime runtime;

  // Ctrl-C stops the token, which ends the server.
  co_spawn(runtime.get_executor(), serve_notes(port), quoll::async::detached, runtime.get_stop_token());
  runtime.run();
}
