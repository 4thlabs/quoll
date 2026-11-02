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

#ifndef QUOLL_ASYNC_WHEN_ALL_HPP
#define QUOLL_ASYNC_WHEN_ALL_HPP

#include <quoll/async/detail/non_void.hpp>
#include <quoll/async/detail/when_all_awaiter.hpp>
#include <quoll/async/task.hpp>

#include <tuple>
#include <utility>

namespace quoll::async {

/**
 * @brief Runs tasks concurrently on the awaiter's context and completes once all of them have.
 *
 * The children start in order, each running until its first suspension. The result holds their values in the same
 * order, with std::monostate for a `task<>`. The children share a stop token that follows the awaiter's, and that
 * is also stopped when a child throws. All of them still run to completion, then the first exception is rethrown.
 */
template <class... Ts>
[[nodiscard]] task<std::tuple<detail::non_void_t<Ts>...>> when_all(task<Ts>... children) {
  detail::when_all_awaiter<Ts...> awaiter{std::move(children)...};
  co_return co_await awaiter;
}

} // namespace quoll::async

#endif // QUOLL_ASYNC_WHEN_ALL_HPP
