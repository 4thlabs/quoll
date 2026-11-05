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

#ifndef QUOLL_ASYNC_DETAIL_RECYCLED_FRAME_HPP
#define QUOLL_ASYNC_DETAIL_RECYCLED_FRAME_HPP

#include <cstddef>

namespace quoll::async::detail {

// Memory for a coroutine frame, from what this thread freed before if it can: a task per operation then allocates
// nothing in steady state. Thread safety: any thread, as a frame freed on a thread goes to that thread's frames.
[[nodiscard]] void *allocate_frame(std::size_t size);
void deallocate_frame(void *frame, std::size_t size) noexcept;

// False when the library is built under a sanitizer, for it to see each frame come and go: what uses one after its end
// would go unseen in a frame that lives on. An application under one, against a library built without, and Valgrind, do
// not see them.
[[nodiscard]] bool frames_are_recycled() noexcept;

// What the promise of a coroutine derives from for its frame to be recycled.
class recycled_frame {
public:
  [[nodiscard]] static void *operator new(std::size_t size) { return allocate_frame(size); }

  static void operator delete(void *frame, std::size_t size) noexcept { deallocate_frame(frame, size); }
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_RECYCLED_FRAME_HPP
