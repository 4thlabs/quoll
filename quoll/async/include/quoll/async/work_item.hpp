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

#ifndef QUOLL_ASYNC_WORK_ITEM_HPP
#define QUOLL_ASYNC_WORK_ITEM_HPP

#include <coroutine>

namespace quoll::async {

/**
 * @brief What an executor runs, linked into its queue instead of allocated: a coroutine to resume, or a function to
 * call.
 *
 * Owned by the code that queues it, usually an awaitable living in the coroutine frame, and lent to the executor by
 * `post`. Until the executor has run it, the item keeps a stable address, is not modified, and is in no other queue:
 * post it at most once at a time. The executor runs it with execute(), which reads what it needs before running it,
 * as running it may destroy the item; `next` belongs to the executor.
 *
 * `run`, when set, is called in place of resuming `handle`. A queue cannot take an item back, so whatever may be
 * destroyed before its turn comes (a coroutine frame, while it is suspended) posts an item that outlives it instead,
 * whose `run` looks at that time whether there is still something to resume: see io::memory_stream.
 */
struct work_item {
  std::coroutine_handle<> handle;
  work_item *next = nullptr;
  void (*run)(work_item &item) = nullptr;
};

/**
 * @brief Runs `item` as an executor does: calls `item.run` when it is set, otherwise resumes `item.handle`. What a
 * resumed coroutine lets escape goes on to the executor's caller.
 */
inline void execute(work_item &item) {
  if (auto *const run = item.run) {
    run(item);
    return;
  }

  // Read before resuming: the item may live in the frame the coroutine destroys.
  auto const handle = item.handle;

  handle.resume();
}

} // namespace quoll::async

#endif // QUOLL_ASYNC_WORK_ITEM_HPP
