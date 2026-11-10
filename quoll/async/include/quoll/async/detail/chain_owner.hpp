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

#ifndef QUOLL_ASYNC_DETAIL_CHAIN_OWNER_HPP
#define QUOLL_ASYNC_DETAIL_CHAIN_OWNER_HPP

#include <quoll/async/executor_ref.hpp>
#include <quoll/async/work_item.hpp>

#include <atomic>
#include <coroutine>
#include <cstdio>
#include <exception>
#include <optional>

namespace quoll::async::detail {

// The owner of a chain of coroutines awaiting each other: the coroutine that started its top task, suspended above it
// until that task completes. Every coroutine of the chain reaches it through its await_context.
//
// What ends on a stop resumes nothing of the chain: it calls stop(), which posts the owner. Resumed, the owner finds
// the chain stopped, and unwinds it from the top: it destroys the frame it awaits, which destroys the frames below it,
// their locals with them, without any coroutine of the chain resuming. No catch can then see the stop, as no code
// after a co_await of the chain runs. Capy's chain_owner does this for a coroutine given up (abandon), as
// std::execution's set_stopped does for a stop.
//
// Thread safety: stop() from any thread, once per suspension of the chain, which waits at one point only; a second
// call does nothing. The rest on the owner's executor.
class chain_owner {
public:
  chain_owner() = default;
  chain_owner(chain_owner const &) = delete;
  chain_owner(chain_owner &&) = delete;
  chain_owner &operator=(chain_owner const &) = delete;
  chain_owner &operator=(chain_owner &&) = delete;
  ~chain_owner() = default;

  // Before the chain starts: the coroutine to resume, and where.
  void bind(std::coroutine_handle<> self, executor_ref executor) noexcept {
    self_.handle = self;
    executor_.emplace(executor);
  }

  // The chain ended on a stop: posts the owner, which unwinds it. Nothing of the owner is touched after the post, which
  // may resume it on another thread at once, and free it. A post that throws calls std::terminate, as nothing would
  // be left to resume the chain.
  void stop() noexcept {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    executor_->post(self_);
  }

  [[nodiscard]] bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

  // From now on the frames of the chain are destroyed suspended, which a task's promise allows only then.
  void unwind() noexcept { unwinding_ = true; }

  [[nodiscard]] bool unwinding() const noexcept { return unwinding_; }

private:
  std::atomic<bool> stopped_{false};
  bool unwinding_ = false;
  work_item self_{};
  std::optional<executor_ref> executor_;
};

// Stops the chain `owner` owns. One with none ends the program in every build, as nothing could unwind it, rather than
// leave it suspended for ever: an await_context Quoll did not make, under an operation that ends on a stop.
inline void stop_chain(chain_owner *owner) noexcept {
  if (owner == nullptr) {
    std::fputs("quoll::async: a chain ended on a stop, with no owner to unwind it\n", stderr);
    std::terminate();
  }

  owner->stop();
}

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_CHAIN_OWNER_HPP
