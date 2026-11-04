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

#ifndef QUOLL_UI_EVENT_FUNCTION_HPP
#define QUOLL_UI_EVENT_FUNCTION_HPP

#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/task.hpp>

#include <concepts>
#include <coroutine>
#include <string>
#include <string_view>
#include <type_traits>

namespace quoll::ui {

namespace detail {

// Nothing, or what can be awaited: any other result would be dropped without a word, even an awaitable never started.
template <class R>
concept EventResult = std::is_void_v<R> || async::ContextAwaitable<R>;

template <class F, class... Args>
concept EventCallable = std::invocable<F &, Args...> && EventResult<std::invoke_result_t<F &, Args...>>;

} // namespace detail

/**
 * @brief What can run on an event: a function taking the node's value or nothing, copied for each event, and returning
 * nothing or something to await.
 */
template <class F>
concept EventFunction =
    std::copy_constructible<F> && (detail::EventCallable<F, std::string> || detail::EventCallable<F>);

static_assert(EventFunction<void (*)()>);
static_assert(EventFunction<async::task<> (*)()>);
static_assert(EventFunction<async::task<int> (*)(std::string)>);
static_assert(!EventFunction<int (*)()>);
static_assert(!EventFunction<std::suspend_always (*)()>);
static_assert(EventFunction<void (*)(std::string)>);
static_assert(EventFunction<void (*)(std::string_view)>);
static_assert(EventFunction<void (*)(std::string const &)>);
static_assert(!EventFunction<void (*)(int)>);
static_assert(!EventFunction<void (*)(std::string &)>);

} // namespace quoll::ui

#endif // QUOLL_UI_EVENT_FUNCTION_HPP
