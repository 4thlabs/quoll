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

#ifndef QUOLL_ASYNC_DETAIL_CONTEXT_ADAPTER_HPP
#define QUOLL_ASYNC_DETAIL_CONTEXT_ADAPTER_HPP

#include <quoll/async/await_context.hpp>

#include <coroutine>
#include <type_traits>
#include <utility>

namespace quoll::async::detail {

// Standard awaitable binding a ContextAwaitable to a context. `A` is a reference type when an lvalue is awaited (the
// awaitable outlives the co_await) and a value type when a temporary is: the temporary is moved in.
template <class A>
class context_adapter {
public:
  context_adapter(A &&awaitable, await_context const &context) noexcept(std::is_nothrow_constructible_v<A, A &&>)
      : awaitable_(std::forward<A>(awaitable)), context_(&context) {}

  [[nodiscard]] bool await_ready() { return awaitable_.await_ready(); }

  decltype(auto) await_suspend(std::coroutine_handle<> handle) { return awaitable_.await_suspend(handle, *context_); }

  decltype(auto) await_resume() { return awaitable_.await_resume(); }

private:
  A awaitable_;
  await_context const *context_;
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_CONTEXT_ADAPTER_HPP
