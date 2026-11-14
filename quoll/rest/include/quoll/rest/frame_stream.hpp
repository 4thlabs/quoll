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

#ifndef QUOLL_REST_FRAME_STREAM_HPP
#define QUOLL_REST_FRAME_STREAM_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/detail/one_at_a_time.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/response_body.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/detail/refusal.hpp>
#include <quoll/rest/framing.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace quoll::rest {

/**
 * @brief The values of a route of stream<F>, as a client's `subscribe` gives them, each cut by `F` from what comes. A
 * value may view the bytes it was read from, which stay until the next next().
 *
 * One next() at a time. Bytes that cut no value throw an http::http_error received from next(), holding them and what
 * followed as its body, and end the stream, as a stream that ends inside a value does: past bytes that are none, no
 * frame can be told from the next. Destroyed before its end is how to give up on it.
 */
template <Framing F>
class frame_stream {
public:
  using value_type = typename F::value_type;

  /** @brief Reads `body`; a client's `subscribe` makes one. */
  explicit frame_stream(http::response_body body) : body_(std::move(body)) {}

  /** @brief The next value, or nothing once the stream has ended. */
  [[nodiscard]] async::task<std::optional<value_type>> next() {
    if (!state_) {
      throw std::logic_error{"quoll::rest::frame_stream: next() once moved from"};
    }

    http::detail::one_at_a_time turn{
        std::shared_ptr<std::atomic<bool>>{state_, &state_->busy},
        "quoll::rest::frame_stream: next() while another is pending"
    };

    return reading(std::move(turn), body_, state_);
  }

private:
  // What has come, the bytes the last value was read from at its front, kept until the next next() as the value may
  // view them; whether the body has ended, and the turn of one next() at a time.
  struct state {
    std::string pending;
    std::size_t taken = 0;
    bool ended = false;
    std::atomic<bool> busy = false;
  };

  [[noreturn]] static void refuse(std::string bytes, std::string reason) {
    http::response received;

    received.status = http::status::ok;
    received.body = std::move(bytes);
    detail::fail(detail::refusal{std::move(received), std::move(reason)});
  }

  // No `this`: the body and what has come are the task's, a copy of the body sharing it.
  [[nodiscard]] static async::task<std::optional<value_type>>
  reading(http::detail::one_at_a_time turn, http::response_body body, std::shared_ptr<state> kept) {
    auto const held = std::move(turn);

    // The last value's bytes go now, as next() is asked again: its views end with it.
    kept->pending.erase(0, std::exchange(kept->taken, 0));

    for (;;) {
      auto value = F{}.decode(kept->pending);

      if (!value) {
        // Ended: what follows bytes that are none cannot be told apart.
        kept->ended = true;
        refuse(std::exchange(kept->pending, {}), "invalid frame: " + value.error().message);
      }

      if (*value) {
        kept->taken = (*value)->size;
        co_return std::move((*value)->value);
      }

      if (kept->ended) {
        if (!kept->pending.empty()) {
          refuse(std::exchange(kept->pending, {}), "a stream ended inside a frame");
        }

        co_return std::nullopt;
      }

      auto const piece = co_await body.read();

      if (piece) {
        kept->pending.append(*piece);
      } else {
        kept->ended = true;
      }
    }
  }

  http::response_body body_;
  std::shared_ptr<state> state_ = std::make_shared<state>();
};

} // namespace quoll::rest

#endif // QUOLL_REST_FRAME_STREAM_HPP
