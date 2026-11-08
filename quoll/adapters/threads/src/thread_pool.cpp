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

#include <quoll/threads/thread_pool.hpp>

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace quoll::threads {

namespace {

// The stack holds the most recent post first: reversed, it gives the posting
// order back.
[[nodiscard]] async::work_item *reverse(async::work_item *stack) noexcept {
  async::work_item *queue = nullptr;

  while (stack != nullptr) {
    auto *const item = stack;

    stack = item->next;
    item->next = queue;
    queue = item;
  }

  return queue;
}

} // namespace

thread_pool::thread_pool() : thread_pool(std::max(1U, std::thread::hardware_concurrency())) {}

thread_pool::thread_pool(std::size_t threads) {
  if (threads == 0) {
    throw std::invalid_argument{"quoll::threads::thread_pool needs at least one thread"};
  }

  workers_.reserve(threads);

  try {
    for (std::size_t index = 0; index < threads; ++index) {
      workers_.emplace_back([this](std::stop_token const &token) { work(token); });
    }
  } catch (...) {
    // The workers already started would otherwise wait forever, and their
    // destructors join them.
    stop();
    throw;
  }
}

thread_pool::~thread_pool() {
  stop();

  for (auto &worker : workers_) {
    assert(worker.get_id() != std::this_thread::get_id() && "thread_pool destroyed by one of its workers");
    worker.join();
  }

  assert(incoming_.load() == nullptr && ready_.with([](async::work_item const *const &ready) {
    return ready == nullptr;
  }) && "thread_pool posted to after its destruction started");
}

void thread_pool::push(async::work_item &item) noexcept {
  item.next = incoming_.load();

  // A failed exchange reloads the current head into item.next: retry on top of it.
  while (!incoming_.compare_exchange_weak(item.next, &item)) {
  }

  // A worker that found no work waits for the epoch it saw to change.
  epoch_.fetch_add(1);
  epoch_.notify_one();
}

// The next item to run, or null when nothing is queued. Out of the queue, it is this worker's alone.
async::work_item *thread_pool::pop() {
  return ready_.with([this](async::work_item *&ready) -> async::work_item * {
    if (ready == nullptr) {
      ready = reverse(incoming_.exchange(nullptr));
    }

    if (ready == nullptr) {
      return nullptr;
    }

    auto *const item = ready;

    ready = item->next;
    return item;
  });
}

void thread_pool::work(std::stop_token const &token) {
  for (;;) {
    // Read before looking for work: a post after this point changes the
    // epoch, so wait() does not miss it.
    auto const seen = epoch_.load();

    if (auto *const item = pop()) {
      async::execute(*item);
      continue;
    }

    if (token.stop_requested()) {
      return;
    }

    epoch_.wait(seen);
  }
}

void thread_pool::stop() noexcept {
  for (auto &worker : workers_) {
    worker.request_stop();
  }

  epoch_.fetch_add(1);
  epoch_.notify_all();
}

} // namespace quoll::threads
