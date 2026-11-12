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

#include <quoll/http/detail/one_at_a_time.hpp>
#include <quoll/http/event_writer.hpp>
#include <quoll/http/format_event.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace quoll::http {

event_writer::event_writer(response_writer &&out) {
  // Before it is taken: refused, the handler keeps it, to finish what it started.
  if (!out.writable() || out.head_given()) {
    throw std::logic_error{"quoll::http::event_writer: a response_writer whose head is given, or moved from"};
  }

  // What may throw (the copy, the fields, the allocation) runs before `out` is taken, so that it leaves it as it was.
  auto fields = out.fields;

  // A handler's own, a charset say, stays.
  if (!fields.contains("Content-Type")) {
    fields.set("Content-Type", "text/event-stream");
  }

  if (!fields.contains("X-Accel-Buffering")) {
    fields.set("X-Accel-Buffering", "no");
  }

  auto taken = std::make_shared<state>(std::move(out));

  taken->out.fields = std::move(fields);
  state_ = std::move(taken);
}

// An empty piece sends the head.
async::task<> event_writer::open() {
  check_writable();
  return ask(std::string{});
}

// Moved from is checked before the text is made, so that it is what is reported.
async::task<> event_writer::send(server_sent_event const &event) {
  check_writable();
  return ask(format_event(event));
}

async::task<> event_writer::retry(std::chrono::milliseconds delay) {
  check_writable();
  return ask(format_retry(delay));
}

async::task<> event_writer::comment(std::string_view text) {
  check_writable();
  return ask(format_comment(text));
}

void event_writer::check_writable() const {
  if (!state_) {
    throw std::logic_error{"quoll::http::event_writer: used once moved from"};
  }
}

// What asks has checked the writer.
async::task<> event_writer::ask(std::string text) {
  detail::one_at_a_time turn{
      std::shared_ptr<std::atomic<bool>>{state_, &state_->busy},
      "quoll::http::event_writer: an operation while another is pending"
  };

  return write(state_, std::move(turn), std::move(text));
}

async::task<> event_writer::write(std::shared_ptr<state> writing, detail::one_at_a_time turn, std::string text) {
  // Given back when the operation is over, not when its task goes, which a caller may keep: the parameter covers a task
  // dropped unrun.
  auto const held = std::move(turn);

  co_await writing->out.write(text);
}

} // namespace quoll::http
