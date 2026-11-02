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

#ifndef QUOLL_ASYNC_DETAIL_EXECUTOR_VTABLE_HPP
#define QUOLL_ASYNC_DETAIL_EXECUTOR_VTABLE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/executor.hpp>

#include <coroutine>

namespace quoll::async::detail {

struct executor_vtable {
  void (*post)(void const *executor, std::coroutine_handle<> handle);
  bool (*equal)(void const *lhs, void const *rhs) noexcept;
  type_id type;
};

template <Executor E>
inline constexpr executor_vtable executor_vtable_for{
    [](void const *executor, std::coroutine_handle<> handle) { static_cast<E const *>(executor)->post(handle); },
    [](void const *lhs, void const *rhs) noexcept {
      return *static_cast<E const *>(lhs) == *static_cast<E const *>(rhs);
    },
    type_id_of<E>(),
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_EXECUTOR_VTABLE_HPP
