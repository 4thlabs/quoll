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

#include <quoll/http/event_writer.hpp>
#include <quoll/http/format_event.hpp>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace quoll::http {

event_writer::event_writer(response_writer out) : out_(std::move(out)) {
  if (!out_.writable() || out_.head_given()) {
    throw std::logic_error{"quoll::http::event_writer: a response_writer whose head is given, or moved from"};
  }

  // A handler's own, with a charset say, stays.
  if (!out_.fields.contains("Content-Type")) {
    out_.fields.set("Content-Type", "text/event-stream");
  }

  out_.fields.set("X-Accel-Buffering", "no");
}

async::task<> event_writer::open() {
  check_writable();
  return out_.write(std::string_view{});
}

async::task<> event_writer::send(server_sent_event const &event) { return write(format_event(event)); }

async::task<> event_writer::retry(std::chrono::milliseconds delay) { return write(format_retry(delay)); }

async::task<> event_writer::comment(std::string_view text) { return write(format_comment(text)); }

async::task<> event_writer::write(std::string text) {
  check_writable();
  co_await out_.write(text);
}

void event_writer::check_writable() const {
  if (!out_.writable()) {
    throw std::logic_error{"quoll::http::event_writer: used once moved from"};
  }
}

} // namespace quoll::http
