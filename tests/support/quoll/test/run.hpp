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

#ifndef QUOLL_TEST_RUN_HPP
#define QUOLL_TEST_RUN_HPP

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/test/overloaded.hpp>

#include <exception>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace quoll::test {

/**
 * @brief Runs `work` on `context` (a run_loop, a virtual_time) until the context is drained. Returns its value, or
 * nothing if it never completed (it waits for something that did not come); rethrows its exception, so that Catch2
 * reports it.
 */
template <class Context, class T>
  requires(!std::is_void_v<T>)
std::optional<T> run(Context &context, async::task<T> work, std::stop_token stop_token = {}) {
  std::optional<T> result;
  std::exception_ptr failure;

  co_spawn(context.get_executor(), std::move(work),
           overloaded{[&](T value) { result = std::move(value); },
                      [&](std::exception_ptr error) { failure = std::move(error); }},
           std::move(stop_token));
  context.run();

  // Rethrown here: the handler itself must not throw.
  if (failure) {
    std::rethrow_exception(failure);
  }

  return result;
}

/** @brief run() on a fresh run_loop. */
template <class T>
  requires(!std::is_void_v<T>)
std::optional<T> run(async::task<T> work) {
  async::run_loop loop;

  return run(loop, std::move(work));
}

} // namespace quoll::test

#endif // QUOLL_TEST_RUN_HPP
