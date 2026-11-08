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

#ifndef QUOLL_ASYNC_OFFLOAD_HPP
#define QUOLL_ASYNC_OFFLOAD_HPP

#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/offloader_ref.hpp>
#include <quoll/async/task.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <optional>
#include <stop_token>
#include <system_error>
#include <type_traits>
#include <utility>

namespace quoll::async {

/**
 * @brief Runs `work`, which blocks, away from the event loop of the executor awaiting it, and gives what it returns or
 * throws: reading a file, for instance, which would otherwise hold up every connection of the loop.
 *
 * @code
 * auto const size = co_await quoll::async::offload([&] { return ::pread(file, buffer, length, offset); });
 * @endcode
 *
 * Through the executor's async::offloader_ref, as quoll::runtime offers one. On an executor that offers none, which has
 * no loop to hold up or no other thread to go to (run_loop, a browser's event loop), `work` runs right there.
 * `work` and what it refers to must not need the loop's thread: what it touches, nothing else touches until the task
 * completes. A stop request cancels it before it starts, with
 * std::system_error (std::errc::operation_canceled), on any executor, and does not interrupt it once started.
 */
template <class F>
  requires std::invocable<F &>
[[nodiscard]] task<std::invoke_result_t<F &>> offload(F work) {
  using result = std::invoke_result_t<F &>;

  auto const executor = co_await current_executor();
  auto const offloader = executor.template query<offloader_ref>();

  if (!offloader) {
    // As what an offloader does with what has not started.
    std::stop_token const stop_token = co_await current_stop_token();

    if (stop_token.stop_requested()) {
      throw std::system_error{std::make_error_code(std::errc::operation_canceled), "quoll::async::offload"};
    }

    co_return std::invoke(work);
  }

  if constexpr (std::is_void_v<result>) {
    struct state {
      F *work;
      std::exception_ptr failure{};
    } running{&work};

    co_await offloader->offload(
        [](void *erased) noexcept {
          auto &self = *static_cast<state *>(erased);

          try {
            std::invoke(*self.work);
          } catch (...) {
            self.failure = std::current_exception();
          }
        },
        &running);

    if (running.failure) {
      std::rethrow_exception(running.failure);
    }
  } else {
    struct state {
      F *work;
      std::optional<result> value{};
      std::exception_ptr failure{};
    } running{&work};

    co_await offloader->offload(
        [](void *erased) noexcept {
          auto &self = *static_cast<state *>(erased);

          try {
            self.value.emplace(std::invoke(*self.work));
          } catch (...) {
            self.failure = std::current_exception();
          }
        },
        &running);

    if (running.failure) {
      std::rethrow_exception(running.failure);
    }

    co_return std::move(*running.value);
  }
}

} // namespace quoll::async

#endif // QUOLL_ASYNC_OFFLOAD_HPP
