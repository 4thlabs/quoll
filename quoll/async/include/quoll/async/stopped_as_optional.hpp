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

#ifndef QUOLL_ASYNC_STOPPED_AS_OPTIONAL_HPP
#define QUOLL_ASYNC_STOPPED_AS_OPTIONAL_HPP

#include <quoll/async/detail/owned_chain.hpp>
#include <quoll/async/task.hpp>

#include <utility>

namespace quoll::async {

/**
 * @brief Awaits `work`, and gives `std::nullopt` if it ended on a stop, rather than end the awaiter as well.
 *
 * A stop ends a task without an exception: what it awaits does not resume it, no `catch` sees it, and its frames are
 * destroyed, their locals with them, up to the nearest coroutine that asked to see it, here or at a co_spawn root. Ask
 * for it where something must still run once stopped: tasks to join, an answer to send.
 *
 * @code
 * auto const accepted = co_await quoll::async::stopped_as_optional(listener.accept());
 *
 * if (!accepted) {
 *   co_await connections.join();  // runs: the stop ended accept(), not this coroutine
 *   co_return;
 * }
 * @endcode
 *
 * Gives what `work` returns, `std::monostate` for a `task<>`, and rethrows what it throws. `work` runs on the awaiter's
 * executor, under its stop token. Awaited as a temporary, `co_await std::move(seen)` for one held by name, which a
 * static_assert asks for, once. A task that ends on std::errc::operation_canceled while its stop is requested ends on
 * the stop too.
 */
template <class T>
[[nodiscard]] detail::owned_chain<T> stopped_as_optional(task<T> work) noexcept {
  return detail::owned_chain<T>{std::move(work)};
}

} // namespace quoll::async

#endif // QUOLL_ASYNC_STOPPED_AS_OPTIONAL_HPP
