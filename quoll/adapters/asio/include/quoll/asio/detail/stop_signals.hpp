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

#ifndef QUOLL_ASIO_DETAIL_STOP_SIGNALS_HPP
#define QUOLL_ASIO_DETAIL_STOP_SIGNALS_HPP

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#if !defined(_WIN32)
#include <signal.h>
#endif

#include <array>
#include <csignal>
#include <cstddef>
#include <stop_token>
#include <system_error>

namespace quoll::asio::detail {

// The action of a signal, saved and given back. POSIX keeps the whole sigaction (flags, mask); Windows has signal()
// only, which can read the handler only by setting another for an instant.
#if defined(_WIN32)
using signal_action = void (*)(int);

[[nodiscard]] inline signal_action current_action(int number) noexcept {
  auto const previous = std::signal(number, SIG_DFL);

  std::signal(number, previous);
  return previous;
}

inline void set_action(int number, signal_action const &action) noexcept { std::signal(number, action); }

[[nodiscard]] inline bool ignores(signal_action const &action) noexcept { return action == SIG_IGN; }
#else
using signal_action = struct sigaction;

[[nodiscard]] inline signal_action current_action(int number) noexcept {
  signal_action action{};

  ::sigaction(number, nullptr, &action);
  return action;
}

inline void set_action(int number, signal_action const &action) noexcept { ::sigaction(number, &action, nullptr); }

[[nodiscard]] inline bool ignores(signal_action const &action) noexcept { return action.sa_handler == SIG_IGN; }
#endif

// SIGINT and SIGTERM turned into a stop request while a runtime runs, unless the process ignores them (nohup, a
// background job). Their actions from before are saved, and given back by release(): at the first signal, so that the
// next one has its usual effect (ending a process whose tasks do not stop), and once the tasks are done.
class stop_signals {
public:
  stop_signals(::asio::io_context &context, std::stop_source &stop) : signals_(context), stop_(&stop) {
    for (std::size_t index = 0; index < numbers.size(); ++index) {
      saved_[index] = current_action(numbers[index]);
      taken_[index] = !ignores(saved_[index]);

      if (taken_[index]) {
        signals_.add(numbers[index]);
      }
    }

    signals_.async_wait([this](std::error_code error, int) {
      if (!error) {
        stop_->request_stop();
        release();
      }
    });
  }

  stop_signals(stop_signals const &) = delete;
  stop_signals(stop_signals &&) = delete;
  stop_signals &operator=(stop_signals const &) = delete;
  stop_signals &operator=(stop_signals &&) = delete;

  ~stop_signals() { release(); }

  [[nodiscard]] bool released() const noexcept { return released_; }

  void release() noexcept {
    if (released_) {
      return;
    }

    std::error_code ignored;

    // Ends the wait, which would keep the loop running; then asio sets the default action as it lets the signals go,
    // and the saved one replaces it.
    signals_.cancel(ignored);
    signals_.clear(ignored);

    for (std::size_t index = 0; index < numbers.size(); ++index) {
      if (taken_[index]) {
        set_action(numbers[index], saved_[index]);
      }
    }

    released_ = true;
  }

private:
  static constexpr std::array<int, 2> numbers{SIGINT, SIGTERM};

  ::asio::signal_set signals_;
  std::stop_source *stop_;
  std::array<signal_action, 2> saved_{};
  std::array<bool, 2> taken_{};
  bool released_ = false;
};

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_STOP_SIGNALS_HPP
