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

#ifndef QUOLL_IO_DETAIL_LISTENER_VTABLE_HPP
#define QUOLL_IO_DETAIL_LISTENER_VTABLE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/bound_listener.hpp>
#include <quoll/io/listener.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace quoll::io::detail {

struct listener_vtable {
  async::task<any_stream> (*accept)(void *listener);
  // Empty for a listener that is not a BoundListener.
  std::optional<std::uint16_t> (*local_port)(void const *listener);
  void (*destroy)(void *listener) noexcept;
  async::detail::type_id type;
};

template <Listener L>
inline constexpr listener_vtable listener_vtable_for{
    [](void *listener) { return static_cast<L *>(listener)->accept(); },
    [](void const *listener) -> std::optional<std::uint16_t> {
      if constexpr (BoundListener<L>) {
        return static_cast<L const *>(listener)->local_port();
      } else {
        return std::nullopt;
      }
    },
    [](void *listener) noexcept {
      // Owned from here: the unique_ptr frees the listener.
      std::unique_ptr<L> const owned{static_cast<L *>(listener)};
    },
    async::detail::type_id_of<L>(),
};

} // namespace quoll::io::detail

#endif // QUOLL_IO_DETAIL_LISTENER_VTABLE_HPP
