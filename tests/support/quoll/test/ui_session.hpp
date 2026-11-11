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

#ifndef QUOLL_TEST_UI_SESSION_HPP
#define QUOLL_TEST_UI_SESSION_HPP

#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>

#include <chrono>
#include <exception>

namespace quoll::test {

/**
 * @brief Starts `the_app`, runs `act`, then stops it, even when `act` throws, which it then throws again: all of it
 * within one run of the clock, and the app's tasks over before it goes.
 */
template <class App, class F>
[[nodiscard]] async::task<int> session(App &the_app, F act) {
  std::exception_ptr failure;

  the_app.start();

  try {
    co_await act();
  } catch (...) {
    failure = std::current_exception();
  }

  co_await the_app.stop();

  if (failure) {
    std::rethrow_exception(failure);
  }

  co_return 0;
}

/** @brief Lets what is pending run, on a timer: a virtual_time jumps to it. */
[[nodiscard]] inline async::task<> settle() { co_await async::sleep_for(std::chrono::milliseconds{1}); }

} // namespace quoll::test

#endif // QUOLL_TEST_UI_SESSION_HPP
