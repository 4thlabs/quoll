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

#ifndef QUOLL_IO_DETAIL_KEEPING_HPP
#define QUOLL_IO_DETAIL_KEEPING_HPP

#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>

#include <memory>
#include <utility>

namespace quoll::io::detail {

// A connection's task, and what it was given: a network may take a view of it, in a coroutine that only starts later,
// and refer to it until its frame goes. The members go in reverse order, the task first, which the parameters of a
// coroutine do not promise: GCC 13 destroys them in another. On the heap, for what the task refers to not to move.
template <class Kept>
struct kept_connection {
  std::unique_ptr<Kept> kept;
  async::task<any_stream> connecting;
};

// The task of `owned`, its arguments kept until it has ended, or until it goes without having run.
template <class Kept>
[[nodiscard]] async::task<any_stream> keeping(kept_connection<Kept> owned) {
  co_return co_await std::move(owned.connecting);
}

} // namespace quoll::io::detail

#endif // QUOLL_IO_DETAIL_KEEPING_HPP
