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

#ifndef QUOLL_ASYNC_DETACHED_HPP
#define QUOLL_ASYNC_DETACHED_HPP

#include <exception>
#include <system_error>

namespace quoll::async {

/** @brief Type of async::detached. */
struct detached_t {
  /** @brief A task without a value completed. */
  void operator()() const noexcept {}

  /** @brief A task completed with a value, dropped. */
  template <class T>
  void operator()(T &&) const noexcept {}

  /**
   * @brief A task let an exception escape: it ends the program, as an exception escaping a std::thread does, and the
   * standard library's terminate handler reports it. But for std::errc::operation_canceled, whatever caused it: a stop
   * is no failure, and ends the task alone.
   */
  void operator()(std::exception_ptr failure) const noexcept {
    if (!failure) {
      return;
    }

    try {
      std::rethrow_exception(failure);
    } catch (std::system_error const &thrown) {
      if (thrown.code() != std::errc::operation_canceled) {
        std::rethrow_exception(failure);
      }
    }
  }
};

/**
 * @brief Completion handler for co_spawn() whose task is on its own, like asio::detached: its value is dropped, and an
 * exception it lets escape ends the program, unless it is std::errc::operation_canceled, what a stop request throws.
 *
 * @code
 * co_spawn(runtime.get_executor(), serve_notes(8080), quoll::async::detached, runtime.get_stop_token());
 * @endcode
 */
inline constexpr detached_t detached{};

} // namespace quoll::async

#endif // QUOLL_ASYNC_DETACHED_HPP
