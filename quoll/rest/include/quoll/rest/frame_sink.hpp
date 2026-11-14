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

#ifndef QUOLL_REST_FRAME_SINK_HPP
#define QUOLL_REST_FRAME_SINK_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/response_writer.hpp>
#include <quoll/rest/event_sink.hpp>
#include <quoll/rest/framing.hpp>

#include <memory>
#include <utility>

namespace quoll::rest {

/**
 * @brief What the handler of a route of stream<F> sends its values through, given as its last argument, by reference:
 * as an event_sink does its events, one operation at a time, each awaited before the handler ends.
 *
 * The first operation sends the head, with `F::content_type` unless a middleware set one; an http_error the handler
 * throws before it answers as from any handler, after it the stream is cut. Thread safety: none, as event_sink.
 */
template <Framing F>
class frame_sink {
public:
  using value_type = typename F::value_type;

  /** @brief Writes through `out`; mount() makes one for the handler. */
  explicit frame_sink(http::response_writer &&out) : state_(std::make_shared<detail::event_sink_state>()) {
    state_->out.emplace(std::move(out));
  }

  frame_sink(frame_sink const &) = delete;
  frame_sink &operator=(frame_sink const &) = delete;
  frame_sink(frame_sink &&) = delete;
  frame_sink &operator=(frame_sink &&) = delete;
  ~frame_sink() = default;

  /** @brief Sends the head now, so that a client sees the stream before the first value. */
  [[nodiscard]] async::task<> open() { return detail::frame_open(state_, detail::sink_turn(state_), F::content_type); }

  /** @brief Sends `value`, cut by `F`. @throws what the framing throws, at the call. */
  [[nodiscard]] async::task<> send(value_type const &value) {
    return detail::frame_send(state_, detail::sink_turn(state_), F{}.encode(value), F::content_type);
  }

  /** @brief Whether the stream has started: an operation has run, and an http_error then cuts it rather than answer. */
  [[nodiscard]] bool started() const noexcept { return state_->out && state_->out->head_given(); }

private:
  friend struct detail::event_sink_access;

  [[nodiscard]] async::task<> finished() const { return detail::frame_finish(state_, F::content_type); }

  std::shared_ptr<detail::event_sink_state> state_;
};

} // namespace quoll::rest

#endif // QUOLL_REST_FRAME_SINK_HPP
