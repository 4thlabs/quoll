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
#ifndef QUOLL_ASYNC_DETAIL_EITHER_STOP_HPP
#define QUOLL_ASYNC_DETAIL_EITHER_STOP_HPP

#include <stop_token>

namespace quoll::async::detail {

// What a stop callback runs to stop `source` in turn.
struct stop_waker {
  std::stop_source *source;

  void operator()() const noexcept { source->request_stop(); }
};

// A stop requested once either of two is: stop_scope replaces the stop of what it runs rather than add to it, so that
// two are joined by hand. Neither copied nor moved: its callbacks point to its source. Thread safety: either may be
// stopped from any thread, as a std::stop_callback allows; its destructor waits for a callback running on another one.
class either_stop {
public:
  either_stop(std::stop_token const &first, std::stop_token const &second)
      : on_first_(first, stop_waker{&source_}), on_second_(second, stop_waker{&source_}) {}

  either_stop(either_stop const &) = delete;
  either_stop(either_stop &&) = delete;
  either_stop &operator=(either_stop const &) = delete;
  either_stop &operator=(either_stop &&) = delete;
  ~either_stop() = default;

  [[nodiscard]] std::stop_token token() const noexcept { return source_.get_token(); }

private:
  // Before the callbacks, which may stop it as they are made, and go before it.
  std::stop_source source_;
  std::stop_callback<stop_waker> on_first_;
  std::stop_callback<stop_waker> on_second_;
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_EITHER_STOP_HPP
