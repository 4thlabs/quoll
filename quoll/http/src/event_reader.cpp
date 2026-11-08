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
#include <quoll/http/event_reader.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::http {

event_reader::event_reader(response_body body, std::size_t max_event_size)
    : state_(std::make_shared<state>(std::move(body), event_parser{max_event_size})) {}

async::task<std::optional<server_sent_event>> event_reader::next() {
  if (!state_) {
    throw std::logic_error{"quoll::http::event_reader: next() once moved from"};
  }

  detail::one_at_a_time turn{std::shared_ptr<std::atomic<bool>>{state_, &state_->busy},
                             "quoll::http::event_reader: next() while another is pending"};

  return read_next(state_, std::move(turn));
}

std::string event_reader::last_event_id() const { return state_ ? state_->parser.last_event_id() : std::string{}; }

std::optional<std::chrono::milliseconds> event_reader::retry() const noexcept {
  return state_ ? state_->parser.retry() : std::nullopt;
}

async::task<std::optional<server_sent_event>> event_reader::read_next(std::shared_ptr<state> reading,
                                                                      detail::one_at_a_time turn) {
  if (reading->failure) {
    std::rethrow_exception(reading->failure);
  }

  try {
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
  } catch (std::logic_error const &) {
    // A misuse, a second read of the body from elsewhere say, is the caller's, and ends nothing.
    throw;
  } catch (...) {
    // A read that failed, a stop, a timeout, an event too large, running out of memory: reading ends here, and the
    // connection is not held for a stream nobody reads.
    reading->failure = std::current_exception();
    reading->body = response_body{};
    throw;
  }
}

} // namespace quoll::http
