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

#ifndef QUOLL_ASYNC_EXECUTOR_HPP
#define QUOLL_ASYNC_EXECUTOR_HPP

#include <quoll/async/work_item.hpp>

#include <concepts>

namespace quoll::async {

/**
 * @brief Where and when a suspended coroutine resumes.
 *
 * `post(item)` queues `item.handle` to be resumed later by the executor's context; it never resumes it inline, so an
 * operation may post its awaiter from inside `await_suspend`. The executor links the work_item into its queue rather
 * than allocating. Executors are cheap handles to their context: copyable, and equal when they post to the same
 * context.
 *
 * `post` may throw (e.g. an adapter whose host loop allocates); it has then not queued the item, which the caller
 * still owns. Where the exception cannot propagate, std::terminate is called: when a stop callback, a destructor
 * (see io::memory_stream) or the completion handler of an I/O operation (see quoll::asio) wakes a waiting
 * coroutine. Letting it escape there would leave that coroutine suspended forever. An executor whose `post` cannot
 * fail, like run_loop's, never gets there.
 */
template <class E>
concept Executor = std::copy_constructible<E> && std::equality_comparable<E> &&
                   requires(E const &executor, work_item &item) { executor.post(item); };

} // namespace quoll::async

#endif // QUOLL_ASYNC_EXECUTOR_HPP
