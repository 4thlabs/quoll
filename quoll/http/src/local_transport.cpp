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
#include <quoll/http/detail/final_response.hpp>
#include <quoll/http/detail/late_head.hpp>
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

// What a writer writes, for what wants a body whole, with its Content-Length if it matters: see write_whole_body().
class body_sink {
public:
  body_sink(std::string &body, std::optional<std::uint64_t> announced) noexcept : body_(&body), announced_(announced) {}

  [[nodiscard]] async::task<> write(std::string_view piece) {
    if (failure_) {
      throw std::system_error{failure_, "quoll::http: the body cannot go on"};
    }

    if ((co_await async::current_stop_token()).stop_requested()) {
      failure_ = std::make_error_code(std::errc::operation_canceled);
      throw std::system_error{failure_, "quoll::http: stopped"};
    }

    if (announced_ && body_->size() + piece.size() > *announced_) {
      failure_ = std::make_error_code(std::errc::message_size);
      throw std::length_error{"quoll::http: a body_writer wrote more than its Content-Length"};
    }

    body_->append(piece);
  }

  // What a writer may have made nothing of, for a body cut short not to pass for whole.
  [[nodiscard]] std::error_code failure() const noexcept { return failure_; }

private:
  std::string *body_;
  std::optional<std::uint64_t> announced_;
  std::error_code failure_;
};

// What a write to HEAD throws, told apart from a stop, which is no answer.
struct head_has_no_body : std::system_error {
  head_has_no_body()
      : std::system_error{std::make_error_code(std::errc::operation_canceled),
                          "quoll::http::response_writer: HEAD has no body"} {}
};

// What a handler writing through an http::response_writer writes through. To HEAD, its first write throws: the head is
// all it owes.
class written_by_handler {
public:
  written_by_handler(response &answer, bool head) noexcept
      : answer_(&answer), sink_(answer.body, std::nullopt), head_(head) {}

  void head(status code, fields given) { detail::merge_head(*answer_, code, std::move(given)); }

  [[nodiscard]] async::task<> write(std::string_view piece) {
    if (head_) {
      throw head_has_no_body{};
    }

    co_await sink_.write(piece);
  }

private:
  response *answer_;
  body_sink sink_;
  bool head_;
};

} // namespace

namespace detail {

async::task<> write_whole_body(request &message) {
  if (!message.body.empty()) {
    throw std::invalid_argument{"quoll::http: a request with a body_writer and a body"};
  }

  auto const announced = announced_length(message.fields);
  body_sink sink{message.body, announced};

  co_await message.body_writer(body_stream{sink});
  message.body_writer = nullptr;

  if (auto const failure = sink.failure()) {
    throw std::system_error{failure, "quoll::http: the body was cut short"};
  }

  if ((co_await async::current_stop_token()).stop_requested()) {
    throw std::system_error{std::make_error_code(std::errc::operation_canceled), "quoll::http: stopped"};
  }

  if (announced && *announced != message.body.size()) {
    throw std::length_error{"quoll::http: a body_writer wrote less than its Content-Length"};
  }
}

} // namespace detail

local_transport::local_transport(router target) : router_(std::make_shared<router const>(std::move(target))) {}

async::task<response> local_transport::send_to(std::shared_ptr<router const> target, request message) {
  auto const method = message.method;
  auto const head = method == verb::head;

  // A handler reads what it is given: it is all there before it runs, as with `body`.
  if (message.body_writer) {
    co_await detail::write_whole_body(message);
  }

  // Here rather than left to dispatch(): a writer may read it, and has to find it there.
  auto const body = target->reads_body_in_pieces(message) ? std::exchange(message.body, {}) : std::string{};
  detail::whole_body whole{body};
  auto answer = co_await target->dispatch(std::move(message), body_reader{whole});

  // As a connection answers, and a test here has its failure in hand.
  (void)detail::replace_what_would_mislead(answer, method);

  auto const in_pieces = static_cast<bool>(answer.body_writer);
  auto const body_writer = std::exchange(answer.body_writer, nullptr);
  auto const late_head_writer = std::exchange(answer.late_head_writer, nullptr);

  if (in_pieces && !head) {
    body_sink sink{answer.body, std::nullopt};

    co_await body_writer(body_stream{sink});
  } else if (!in_pieces && late_head_writer) {
    written_by_handler sink{answer, head};

    try {
      co_await late_head_writer(body_stream{sink});
    } catch (head_has_no_body const &) {
      // The head is all it owes.
    }

    (void)detail::replace_what_would_mislead(answer, method);
  }

  // A writer that took a stop for an end has no whole body.
  if (in_pieces || late_head_writer) {
    if ((co_await async::current_stop_token()).stop_requested()) {
      throw std::system_error{std::make_error_code(std::errc::operation_canceled), "quoll::http: stopped"};
    }
  }

  if (head) {
    answer.body.clear();
  }

  co_return answer;
}

} // namespace quoll::http
