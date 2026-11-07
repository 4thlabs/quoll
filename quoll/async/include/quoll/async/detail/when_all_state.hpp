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

#ifndef QUOLL_ASYNC_DETAIL_WHEN_ALL_STATE_HPP
#define QUOLL_ASYNC_DETAIL_WHEN_ALL_STATE_HPP

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <stop_token>
#include <utility>

namespace quoll::async::detail {

// Join point of when_all. Children may complete on other threads: the counter is atomic and the first error is
// published by the release / acquire of the last arrival. The children share a stop source of their own: stopped by
// the first failure, or by the parent's stop token.
class when_all_state {
public:
  // One arrival per child, plus one for the parent once it has started them all.
  explicit when_all_state(std::size_t children) : remaining_(children + 1) {}

  when_all_state(when_all_state const &) = delete;
  when_all_state(when_all_state &&) = delete;
  when_all_state &operator=(when_all_state const &) = delete;
  when_all_state &operator=(when_all_state &&) = delete;
  ~when_all_state() = default;

  void set_parent(std::coroutine_handle<> parent) noexcept { parent_ = parent; }

  void fail(std::exception_ptr error) noexcept {
    if (!failed_.exchange(true, std::memory_order_relaxed)) {
      error_ = std::move(error);
      stop_source_.request_stop();
    }
  }

  void request_stop() noexcept { stop_source_.request_stop(); }

  [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }

  // The coroutine to transfer to: the parent for the last arrival, nothing otherwise.
  [[nodiscard]] std::coroutine_handle<> arrive() noexcept {
    if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      return parent_;
    }

    return std::noop_coroutine();
  }

  void rethrow_if_failed() const {
    if (error_) {
      std::rethrow_exception(error_);
    }
  }

private:
  std::atomic<std::size_t> remaining_;
  std::atomic<bool> failed_{false};
  std::exception_ptr error_;
  std::coroutine_handle<> parent_;
  std::stop_source stop_source_;
};

// Forwards a stop request of the when_all parent to its children.
class when_all_stop_forwarder {
public:
  explicit when_all_stop_forwarder(when_all_state &state) noexcept : state_(&state) {}

  void operator()() const noexcept { state_->request_stop(); }

private:
  when_all_state *state_;
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_WHEN_ALL_STATE_HPP
