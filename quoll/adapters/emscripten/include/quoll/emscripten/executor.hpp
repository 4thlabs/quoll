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

#ifndef QUOLL_EMSCRIPTEN_EXECUTOR_HPP
#define QUOLL_EMSCRIPTEN_EXECUTOR_HPP

#include <quoll/async/executor.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/emscripten/detail/js.hpp>

namespace quoll::emscripten {

/**
 * @brief Executor resuming coroutines on the JavaScript event loop, as microtasks: like the continuation of a
 * JavaScript `await`.
 *
 * The coroutines run between the event loop's tasks, without blocking it: the page keeps rendering and fetch
 * completions keep coming. A coroutine that yields endlessly starves the loop, as an endless chain of promises would.
 * post() never fails and allocates nothing in WebAssembly memory. All executors are equal: there is one event loop.
 *
 * Thread safety: post() from the thread running the event loop (the only one, in a build without pthreads).
 */
class executor {
public:
  void post(async::work_item &item) const noexcept { detail::quoll_emscripten_post(&resume, &item); }

  [[nodiscard]] bool operator==(executor const &) const noexcept = default;

private:
  // Run by the microtask: the item, owned by the suspended coroutine, is alive until then. Its handle is read before
  // the coroutine runs, as the coroutine may destroy the item.
  static void resume(void *item) noexcept {
    auto const handle = static_cast<async::work_item *>(item)->handle;

    handle.resume();
  }
};

static_assert(async::Executor<executor>);

} // namespace quoll::emscripten

#endif // QUOLL_EMSCRIPTEN_EXECUTOR_HPP
