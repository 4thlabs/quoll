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

#include <quoll/http/event_reader.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace quoll::http {

event_reader::event_reader(response_body body, std::size_t max_event_size)
    : state_(std::make_shared<state>(std::move(body), event_parser{max_event_size})) {}

async::task<std::optional<server_sent_event>> event_reader::next() { return read_next(state_); }

std::string event_reader::last_event_id() const { return state_ ? state_->parser.last_event_id() : std::string{}; }

std::optional<std::chrono::milliseconds> event_reader::retry() const noexcept {
  return state_ ? state_->parser.retry() : std::nullopt;
}

async::task<std::optional<server_sent_event>> event_reader::read_next(std::shared_ptr<state> reading) {
  if (!reading) {
    co_return std::nullopt;
  }

  if (reading->reading.exchange(true)) {
    throw std::logic_error{"quoll::http::event_reader: next() while another is pending"};
  }

  // Cleared on every way out, a throw or the frame destroyed while it waits included.
  struct done {
    explicit done(state *reader) noexcept : of(reader) {}

    done(done const &) = delete;
    done &operator=(done const &) = delete;
    done(done &&) = delete;
    done &operator=(done &&) = delete;

    ~done() { of->reading = false; }

    state *of;
  } const clear{reading.get()};

  for (;;) {
    if (auto event = reading->parser.next()) {
      co_return event;
    }

    auto const piece = co_await reading->body.read();

    if (!piece) {
      co_return std::nullopt;
    }

    reading->parser.feed(*piece);
  }
}

} // namespace quoll::http
