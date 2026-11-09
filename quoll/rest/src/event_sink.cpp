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

#include <quoll/http/format_event.hpp>
#include <quoll/rest/event_sink.hpp>

#include <atomic>
#include <stdexcept>
#include <utility>

namespace quoll::rest::detail {

namespace {

[[nodiscard]] http::event_writer &writer_of(event_sink_state &state) {
  if (state.closed.load()) {
    throw std::logic_error{"quoll::rest::event_sink: an operation run once its service has ended"};
  }

  if (!state.events) {
    state.events.emplace(std::move(*state.out));
    state.out.reset();
  }

  return *state.events;
}

} // namespace

http::detail::one_at_a_time sink_turn(std::shared_ptr<event_sink_state> const &state) {
  return http::detail::one_at_a_time{std::shared_ptr<std::atomic<bool>>{state, &state->busy},
                                     "quoll::rest::event_sink: an operation while another is pending"};
}

async::task<> sink_open(std::shared_ptr<event_sink_state> state, http::detail::one_at_a_time turn) {
  auto const held = std::move(turn);

  co_await writer_of(*state).open();
}

async::task<> sink_send(std::shared_ptr<event_sink_state> state, http::detail::one_at_a_time turn,
                        http::server_sent_event event) {
  auto const held = std::move(turn);

  if (!state->events) {
    static_cast<void>(http::format_event(event));
  }

  co_await writer_of(*state).send(event);
}

async::task<> sink_comment(std::shared_ptr<event_sink_state> state, http::detail::one_at_a_time turn,
                           std::string text) {
  auto const held = std::move(turn);

  if (!state->events) {
    static_cast<void>(http::format_comment(text));
  }

  co_await writer_of(*state).comment(text);
}

async::task<> sink_retry(std::shared_ptr<event_sink_state> state, http::detail::one_at_a_time turn,
                         std::chrono::milliseconds delay) {
  auto const held = std::move(turn);

  if (!state->events) {
    static_cast<void>(http::format_retry(delay));
  }

  co_await writer_of(*state).retry(delay);
}

async::task<> sink_answer(std::shared_ptr<event_sink_state> state, http::response refusal) {
  if (!state->out || state->closed.load()) {
    throw std::logic_error{"quoll::rest::event_sink: an answer once the stream has started or ended"};
  }

  auto &out = *state->out;

  out.status = refusal.status;

  for (auto const &field : refusal.fields) {
    out.fields.add(field.name, field.value);
  }

  co_await out.write(refusal.body);
}

async::task<> sink_finish(std::shared_ptr<event_sink_state> state) {
  if (!state->events && !state->busy.load()) {
    co_await sink_open(state, sink_turn(state));
  }

  // An unused response_writer, kept by an operation asked and not run: dropped through an event_writer, so that its
  // head is a stream's, with no event. Then closed.
  if (state->out) {
    http::event_writer const unused{std::move(*state->out)};

    state->out.reset();
  }

  sink_close(*state);
}

void sink_close(event_sink_state &state) noexcept {
  // An operation kept or spawned past the service, not running yet, would write to a connection gone: it throws
  // instead. What writes to the connection goes now, while it is there, rather than with the last such operation. One
  // already writing on another task holds its writer still: past its service, it is the misuse the sink says.
  state.closed = true;

  auto out = std::exchange(state.out, std::nullopt);
  auto events = std::exchange(state.events, std::nullopt);
}

} // namespace quoll::rest::detail
