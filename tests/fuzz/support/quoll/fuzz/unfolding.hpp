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

#ifndef QUOLL_FUZZ_UNFOLDING_HPP
#define QUOLL_FUZZ_UNFOLDING_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/fuzz/support.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <system_error>

namespace quoll::fuzz {

// How the operations of an unfolding_stream go, read from the fuzzer's input: each read, write and shutdown takes the
// next step, and once the steps are used up, every operation completes at once and whole. Shared by the streams of a
// run, which take their steps in the order their operations start.
class script {
public:
  explicit script(std::span<std::uint8_t const> steps) noexcept : steps_(steps) {}

  [[nodiscard]] std::uint8_t next() noexcept {
    if (steps_.empty()) {
      return 0;
    }

    auto const step = steps_.front();

    steps_ = steps_.subspan(1);
    return step;
  }

  // What a step asking for a stop stops: the whole exchange, as a server shutting down or a caller giving up would.
  void stop_with(std::stop_source &source) noexcept { stop_ = &source; }

  void request_stop() const noexcept {
    if (stop_ != nullptr) {
      stop_->request_stop();
    }
  }

private:
  std::span<std::uint8_t const> steps_;
  std::stop_source *stop_ = nullptr;
};

// What an operation does, from the low three bits of its step: most complete at once, the others suspend to the next
// turn of the loop, fail, hang until a stop request ends them, or ask for a stop first.
enum class unfolding : std::uint8_t { at_once, later, failing, hanging };

// A stream whose reads give `input` and whose writes are taken and dropped, unfolding as its script says: a read or a
// write in a piece of any size, now or later, or failing; any operation hanging until a stop, a timeout's or a step's,
// ends it, as a socket's pending operation ends. One completing now or later completes, stop or not. What the code over
// it does on the way out of each state (a timeout firing on a hanging read or write, a stop while it reads and writes
// at once, a reset in the middle of a response) is what this reaches, under the sanitizers. Single-threaded, as
// virtual_time runs it.
class unfolding_stream {
public:
  class [[nodiscard]] awaitable {
  public:
    awaitable(io::io_result result, unfolding how) noexcept : result_(result), how_(how) {}

    awaitable(awaitable const &) = delete;
    awaitable &operator=(awaitable const &) = delete;
    awaitable &operator=(awaitable &&) = delete;

    // Moved only before it suspends, when no stop callback is registered.
    awaitable(awaitable &&other) noexcept : result_(other.result_), how_(other.how_) {}

    ~awaitable() = default;

    [[nodiscard]] bool await_ready() const noexcept { return how_ == unfolding::at_once; }

    void await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
      item_.handle = handle;

      if (how_ == unfolding::hanging) {
        // Only a stop ends it, invoked at once if one was already requested, and it never resumes inline, as it posts.
        // A hang no stop reaches leaves the run unfinished: that is a finding.
        stop_callback_.emplace(context.stop_token, canceller{this, context.executor});
        return;
      }

      context.executor.post(item_);
    }

    [[nodiscard]] io::io_result await_resume() noexcept {
      stop_callback_.reset();
      return result_;
    }

  private:
    struct canceller {
      awaitable *self;
      async::executor_ref executor;

      void operator()() const {
        self->result_ = {std::make_error_code(std::errc::operation_canceled), 0};
        executor.post(self->item_);
      }
    };

    io::io_result result_;
    unfolding how_;
    async::work_item item_{};
    std::optional<std::stop_callback<canceller>> stop_callback_;
  };

  unfolding_stream(script &steps, std::string_view input) noexcept : steps_(&steps), input_(input) {}

  [[nodiscard]] awaitable read_some(std::span<std::byte> buffer) {
    auto const step = take();

    if (step.how == unfolding::failing) {
      return {{failure(step.size), 0}, unfolding::at_once};
    }

    if (step.how == unfolding::hanging) {
      return {{}, unfolding::hanging};
    }

    if (input_.empty()) {
      return {{io::error::end_of_stream, 0}, step.how};
    }

    return {{{}, take_input(input_, buffer, step.size)}, step.how};
  }

  [[nodiscard]] awaitable write_some(std::span<std::byte const> data) {
    auto const step = take();

    if (step.how == unfolding::failing) {
      return {{failure(step.size), 0}, unfolding::at_once};
    }

    if (step.how == unfolding::hanging) {
      return {{}, unfolding::hanging};
    }

    // A short write as often as a whole one: what io::write and the serializers loop on.
    return {{{}, std::min(data.size(), step.size)}, step.how};
  }

  [[nodiscard]] awaitable shutdown_send() {
    auto const step = take();

    if (step.how == unfolding::failing) {
      return {{failure(step.size), 0}, unfolding::at_once};
    }

    return {{}, step.how};
  }

private:
  struct decided {
    unfolding how;
    std::size_t size;
  };

  // The low three bits say how, the others how much: a piece of 1 to 31 bytes, or everything. Bit pattern 6 asks for a
  // stop on the whole exchange, then completes at once.
  decided take() {
    auto const step = steps_->next();
    auto const piece = static_cast<std::size_t>(step >> 3U);
    auto const size = piece == 0 ? std::numeric_limits<std::size_t>::max() : piece;

    switch (step & 7U) {
    case 3:
      return {unfolding::later, size};
    case 4:
      return {unfolding::failing, piece};
    case 5:
      return {unfolding::hanging, size};
    case 6:
      steps_->request_stop();
      return {unfolding::at_once, size};
    default:
      return {unfolding::at_once, size};
    }
  }

  [[nodiscard]] static std::error_code failure(std::size_t which) {
    switch (which % 3) {
    case 0:
      return std::make_error_code(std::errc::connection_reset);
    case 1:
      return std::make_error_code(std::errc::broken_pipe);
    default:
      return std::make_error_code(std::errc::io_error);
    }
  }

  script *steps_;
  std::string_view input_;
};

static_assert(io::HalfClosableStream<unfolding_stream>);

} // namespace quoll::fuzz

#endif // QUOLL_FUZZ_UNFOLDING_HPP
