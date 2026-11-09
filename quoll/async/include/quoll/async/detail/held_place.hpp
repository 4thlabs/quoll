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

#ifndef QUOLL_ASYNC_DETAIL_HELD_PLACE_HPP
#define QUOLL_ASYNC_DETAIL_HELD_PLACE_HPP

#include <quoll/async/detail/slots.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>

#include <utility>

namespace quoll::async::detail {

// A place of some slots, held until it goes or release(). Thread safety: none, as what holds it.
class held_place {
public:
  held_place() noexcept = default;

  held_place(held_place const &) = delete;
  held_place &operator=(held_place const &) = delete;
  held_place(held_place &&) = delete;
  held_place &operator=(held_place &&) = delete;

  ~held_place() { release(); }

  void release() noexcept {
    if (places_ != nullptr) {
      std::exchange(places_, nullptr)->release();
    }
  }

  // A place, if one is free now, held here: see slots::try_acquire(). `places` outlives this.
  [[nodiscard]] bool try_take(slots &places) noexcept {
    if (places_ != nullptr || !places.try_acquire()) {
      return false;
    }

    places_ = &places;
    return true;
  }

  // Owned from the moment it is given, whatever fails around the wait for it: a timeout or a stop that comes as it is
  // given leaves it held here, not lost. `places` outlives this.
  [[nodiscard]] task<> take(slots &places QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    co_await places.acquire();
    places_ = &places;
  }

private:
  slots *places_ = nullptr;
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_HELD_PLACE_HPP
