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
//   quoll_notes_server [port]
//   curl -X POST localhost:8080/api/notes -H 'Content-Type: application/json' -d '{"text":"buy milk"}'
//   curl 'localhost:8080/api/notes?contains=milk'

#include "notes_api.hpp"

#include <quoll/asio.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest.hpp>
#include <quoll/server.hpp>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>

#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace endpoints = notes::endpoints;

// The notes, in memory. The io_context runs on one thread: no lock.
class notes_service {
public:
  std::vector<notes::note> operator()(endpoints::list, notes::note_filter filter) const {
    std::vector<notes::note> found;

    for (auto const &[id, note] : notes_) {
      if (!filter.contains || note.text.contains(*filter.contains)) {
        found.push_back(note);
      }
    }

    return found;
  }

  notes::note operator()(endpoints::get, std::int64_t id) const {
    auto const found = notes_.find(id);

    if (found == notes_.end()) {
      // An error status the endpoint does not declare: answered as an RFC 9457 problem.
      throw quoll::rest::http_error{quoll::http::status::not_found, "no note " + std::to_string(id)};
    }

    return found->second;
  }

  notes::note operator()(endpoints::create, notes::new_note created) {
    notes::note note{.id = next_id_++, .text = std::move(created.text)};

    notes_.emplace(note.id, note);
    return note;
  }

  void operator()(endpoints::remove, std::int64_t id) { notes_.erase(id); }

private:
  std::map<std::int64_t, notes::note> notes_;
  std::int64_t next_id_ = 1;
};

// What becomes of the accept loop.
struct report_end {
  void operator()() const { std::cout << "stopped\n"; }

  void operator()(std::exception_ptr failure) const {
    try {
      std::rethrow_exception(std::move(failure));
    } catch (std::exception const &error) {
      std::cerr << "server failed: " << error.what() << '\n';
    }
  }
};

} // namespace

int main(int argc, char **argv) {
  auto const port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8080);
  ::asio::io_context context;
  quoll::asio::executor const executor{context.get_executor()};
  quoll::asio::acceptor acceptor{::asio::ip::tcp::acceptor{context, {::asio::ip::tcp::v4(), port}}};
  quoll::http::router router;

  quoll::rest::mount<notes::api>(router, notes_service{});

  // Ctrl-C stops the accept loop and the connections.
  std::stop_source stop;
  ::asio::signal_set signals{context, SIGINT, SIGTERM};

  signals.async_wait([&](std::error_code, int) { stop.request_stop(); });
  co_spawn(executor, quoll::server::serve(executor, acceptor, router), report_end{}, stop.get_token());
  std::cout << "notes API on http://localhost:" << port << "/api/notes\n";
  context.run();
}
