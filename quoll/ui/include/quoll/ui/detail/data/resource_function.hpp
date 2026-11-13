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

#ifndef QUOLL_UI_DETAIL_DATA_RESOURCE_FUNCTION_HPP
#define QUOLL_UI_DETAIL_DATA_RESOURCE_FUNCTION_HPP

#include <quoll/async/task.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

namespace quoll::ui::detail {

// What a resource's state holds of its loads: what the last that ended well gave, none before the first, or once what
// it follows holds none.
template <class T>
struct load_result {
  // What it follows holding none, nothing is loaded: its state goes back to before its first load.
  using cleared_when_none = void;

  std::optional<T> loaded;

  [[nodiscard]] bool operator==(load_result const &) const = default;
};

// A run of a resource's function: what it gives, held, so that its state tells a load ended from none yet. The
// function and the values are parameters, in the frame, which outlives the task the function gives.
template <class T, class F, class... V>
async::task<load_result<T>> resource_run(F function, V... values) {
  co_return load_result<T>{.loaded = co_await std::invoke(function, std::move(values)...)};
}

// What the effect of a resource runs: its function, each run's result held. Null when its function is, which the
// effect refuses.
template <class T, class F>
struct resource_function {
  F function;

  template <class... V>
  [[nodiscard]] async::task<load_result<T>> operator()(V &&...values) const {
    return resource_run<T>(function, std::forward<V>(values)...);
  }

  [[nodiscard]] bool operator==(std::nullptr_t) const
    requires requires { function == nullptr; }
  {
    return function == nullptr;
  }
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_DATA_RESOURCE_FUNCTION_HPP
