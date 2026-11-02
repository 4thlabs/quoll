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

#ifndef QUOLL_ASIO_DETAIL_CANCELLATION_HPP
#define QUOLL_ASIO_DETAIL_CANCELLATION_HPP

#include <asio/any_io_executor.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace quoll::asio::detail {

// Lets a stop request, made on any thread, cancel the operation an I/O object has pending, one operation at a time.
//
// asio's cancellation signals are not thread-safe, and neither are its I/O objects. So everything touching the signal
// runs on the I/O object's executor: the start of an operation (start()), its end (finish(), from its handler) and
// its cancellation, which request() posts there. Each operation gets a ticket when it is about to start: a request
// only cancels the operation it was made for, never a later one, nor one that has already finished.
//
// Shared: a posted cancellation keeps it alive, even past the I/O object.
class cancellation {
public:
  explicit cancellation(::asio::any_io_executor executor) noexcept : executor_(std::move(executor)) {}

  cancellation(cancellation const &) = delete;
  cancellation(cancellation &&) = delete;
  cancellation &operator=(cancellation const &) = delete;
  cancellation &operator=(cancellation &&) = delete;
  ~cancellation() = default;

  [[nodiscard]] ::asio::any_io_executor const &executor() const noexcept { return executor_; }

  // Any thread: the ticket of the operation about to start, which makes every earlier request obsolete.
  [[nodiscard]] std::uint64_t next() noexcept { return ++latest_; }

  // On the executor: the slot to bind the operation of `ticket` to.
  [[nodiscard]] ::asio::cancellation_slot start(std::uint64_t ticket) noexcept {
    started_ = ticket;
    return signal_.slot();
  }

  // On the executor, from the operation's handler.
  void finish() noexcept { started_ = 0; }

  // Any thread: cancels the operation of `ticket` if it is still pending once the request reaches the executor. An
  // operation that has not started yet observes the stop token itself when it starts.
  static void request(std::shared_ptr<cancellation> self, std::uint64_t ticket) {
    auto executor = self->executor_;

    ::asio::post(executor, [self = std::move(self), ticket] {
      if (self->started_ == ticket && self->latest_.load() == ticket) {
        self->signal_.emit(::asio::cancellation_type::terminal);
      }
    });
  }

private:
  ::asio::any_io_executor executor_;
  std::atomic<std::uint64_t> latest_{0};
  // Executor only.
  std::uint64_t started_ = 0;
  ::asio::cancellation_signal signal_;
};

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_CANCELLATION_HPP
