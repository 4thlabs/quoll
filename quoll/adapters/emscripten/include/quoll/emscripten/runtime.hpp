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

#ifndef QUOLL_EMSCRIPTEN_RUNTIME_HPP
#define QUOLL_EMSCRIPTEN_RUNTIME_HPP

#include <quoll/async/runtime_options.hpp>
#include <quoll/async/task.hpp>
#include <quoll/emscripten/executor.hpp>

#include <stop_token>

namespace quoll::emscripten {

/**
 * @brief The runtime of a browser, a Worker or Node: the JavaScript event loop, for `main()` to read as it does
 * natively.
 *
 * @code
 * int main() {
 *   quoll::runtime runtime;
 *
 *   return runtime.run(app());
 * }
 * @endcode
 *
 * The loop is the browser's, and goes on once `main()` has returned. So that run() returns at once, before any task has
 * run, where a native one returns once they have ended. What follows it in `main()` runs first, and **a task does not
 * refer to a local of `main()`**, which is gone when it runs, unlike natively: what it uses lives in it. Nothing of the
 * tasks depends on the runtime, which `main()` destroys. The token never stops, as a page has no Ctrl-C.
 *
 * Neither copied nor moved, as a native one. Thread safety: use it from the thread running the event loop.
 */
class runtime {
public:
  using executor = emscripten::executor;
  using executor_type = executor;

  runtime() = default;

  /** @brief As a native one is built: the browser runs one thread, whatever `options` ask. */
  explicit runtime(async::runtime_options options) noexcept;

  runtime(runtime const &) = delete;
  runtime(runtime &&) = delete;
  runtime &operator=(runtime const &) = delete;
  runtime &operator=(runtime &&) = delete;
  ~runtime() = default;

  /** @brief The executor of the event loop, to co_spawn() its tasks on. */
  [[nodiscard]] executor get_executor() noexcept;

  /** @brief A stop token that never stops. */
  [[nodiscard]] std::stop_token get_stop_token() noexcept;

  /** @brief Returns at once, for `main()` to return in turn: the loop is the browser's, and runs from then on. */
  void run() noexcept;

  /**
   * @brief Spawns `main`, the program's task, and returns EXIT_SUCCESS at once, as run() does: the loop runs it once
   * `main()` has returned. What `main` lets escape is reported (async::report()) and, under Node, makes the process
   * exit with a failure, as a native run(task) returns EXIT_FAILURE. For `return runtime.run(program());`.
   */
  [[nodiscard]] int run(async::task<> main);
};

} // namespace quoll::emscripten

#endif // QUOLL_EMSCRIPTEN_RUNTIME_HPP
