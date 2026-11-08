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

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace quoll::http {

event_writer::event_writer(response_writer out) {
  if (!out.writable() || out.head_given()) {
    throw std::logic_error{"quoll::http::event_writer: a response_writer whose head is given, or moved from"};
  }

  // A handler's own, a charset say, stays.
  if (!out.fields.contains("Content-Type")) {
    out.fields.set("Content-Type", "text/event-stream");
  }

  if (!out.fields.contains("X-Accel-Buffering")) {
    out.fields.set("X-Accel-Buffering", "no");
  }

  state_ = std::make_shared<state>(std::move(out));
}

class event_writer::operation {
public:
  explicit operation(std::shared_ptr<state> of) : of_(std::move(of)) {
    if (of_->busy.exchange(true)) {
      of_.reset();
      throw std::logic_error{"quoll::http::event_writer: an operation while another is pending"};
    }
  }

  operation(operation const &) = delete;
  operation &operator=(operation const &) = delete;

  operation(operation &&other) noexcept : of_(std::move(other.of_)) {}

  operation &operator=(operation &&) = delete;

  ~operation() {
    if (of_) {
      of_->busy.store(false);
    }
  }

  [[nodiscard]] response_writer &out() const noexcept { return of_->out; }

private:
  std::shared_ptr<state> of_;
};

// An empty piece sends the head.
async::task<> event_writer::open() {
  operation asked{checked()};

  return write(std::move(asked), std::string{});
}

async::task<> event_writer::send(server_sent_event const &event) {
  auto writing = checked();
  auto text = format_event(event);
  operation asked{std::move(writing)};

  return write(std::move(asked), std::move(text));
}

async::task<> event_writer::retry(std::chrono::milliseconds delay) {
  auto writing = checked();
  auto text = format_retry(delay);
  operation asked{std::move(writing)};

  return write(std::move(asked), std::move(text));
}

async::task<> event_writer::comment(std::string_view text) {
  auto writing = checked();
  auto formatted = format_comment(text);
  operation asked{std::move(writing)};

  return write(std::move(asked), std::move(formatted));
}

std::shared_ptr<event_writer::state> event_writer::checked() const {
  if (!state_) {
    throw std::logic_error{"quoll::http::event_writer: used once moved from"};
  }

  return state_;
}

// `writing` holds the flag from the call: a parameter, destroyed with the frame whether the task ran or not.
async::task<> event_writer::write(operation writing, std::string text) { co_await writing.out().write(text); }

} // namespace quoll::http
