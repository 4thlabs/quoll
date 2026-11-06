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

#include <quoll/http/local_transport.hpp>

#include <quoll/async/current_stop_token.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/detail/streamed_head.hpp>
#include <quoll/http/detail/write_whole_body.hpp>
#include <quoll/http/verb.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace quoll::http {

namespace {

// What a writer writes for a request, for what wants it whole. See write_whole_body().
class request_sink {
public:
  request_sink(std::string &body, std::optional<std::uint64_t> announced) noexcept
      : body_(&body), announced_(announced) {}

  [[nodiscard]] async::task<> write(std::string_view piece) {
    if ((co_await async::current_stop_token()).stop_requested()) {
      throw std::system_error{std::make_error_code(std::errc::operation_canceled), "quoll::http: stopped"};
    }

    if (announced_ && body_->size() + piece.size() > *announced_) {
      throw std::length_error{"quoll::http: a body_writer wrote more than its Content-Length"};
    }

    body_->append(piece);
  }

private:
  std::string *body_;
  std::optional<std::uint64_t> announced_;
};

// What a writer writes, for a caller of the same program that wants a response whole.
class string_sink {
public:
  explicit string_sink(std::string &body) noexcept : body_(&body) {}

  [[nodiscard]] async::task<> write(std::string_view piece) {
    body_->append(piece);
    co_return;
  }

private:
  std::string *body_;
};

} // namespace

namespace detail {

async::task<> write_whole_body(request &message) {
  if (!message.body.empty()) {
    throw std::invalid_argument{"quoll::http: a request with a body_writer and a body"};
  }

  auto const announced = announced_length(message.fields);
  request_sink sink{message.body, announced};

  co_await message.body_writer(body_stream{sink});
  message.body_writer = nullptr;

  if (announced && *announced != message.body.size()) {
    throw std::length_error{"quoll::http: a body_writer wrote less than its Content-Length"};
  }
}

} // namespace detail

async::task<response> local_transport::send_to(std::reference_wrapper<router const> target, request message) {
  auto const head = message.method == verb::head;

  // A handler reads what it is given: it is all there before it runs, as with `body`.
  if (message.body_writer) {
    co_await detail::write_whole_body(message);
  }

  // Here rather than left to dispatch(): a writer may read it, and has to find it there.
  auto const body = target.get().reads_body_in_pieces(message) ? std::exchange(message.body, {}) : std::string{};
  detail::whole_body whole{body};
  auto answer = co_await target.get().dispatch(std::move(message), body_reader{whole});

  if (answer.body_writer && !head) {
    string_sink sink{answer.body};

    co_await answer.body_writer(body_stream{sink});
  }

  answer.body_writer = nullptr;

  if (head) {
    answer.body.clear();
  }

  co_return answer;
}

} // namespace quoll::http
