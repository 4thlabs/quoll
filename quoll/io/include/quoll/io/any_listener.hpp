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

#ifndef QUOLL_IO_ANY_LISTENER_HPP
#define QUOLL_IO_ANY_LISTENER_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/listener.hpp>

#include <cassert>
#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace quoll::io {

namespace detail {

struct listener_vtable {
  async::task<any_stream> (*accept)(void *listener);
  void (*destroy)(void *listener) noexcept;
  async::detail::type_id type;
};

template <Listener L>
inline constexpr listener_vtable listener_vtable_for{
    [](void *listener) { return static_cast<L *>(listener)->accept(); },
    [](void *listener) noexcept {
      // Owned from here: the unique_ptr frees the listener.
      std::unique_ptr<L> const owned{static_cast<L *>(listener)};
    },
    async::detail::type_id_of<L>(),
};

} // namespace detail

/**
 * @brief Owning, type-erased Listener; models Listener itself.
 *
 * Holds any Listener (a TCP acceptor...) behind one type, for code that cannot be a template over it: what a network
 * hands out (see io::listen). The listener is allocated once, when it is wrapped: a server has a few, for its whole
 * life, so a small buffer would buy nothing. Accepting allocates no more than the wrapped listener does.
 *
 * Move-only; it moves without moving the wrapped listener, so a pending accept is not disturbed. A moved-from
 * any_listener can only be assigned to or destroyed.
 *
 * Thread safety: same as the wrapped listener.
 */
class any_listener {
public:
  /** @brief Takes `listener` over. */
  // The exclusion comes first: it stops Listener<any_listener> from checking this constructor against itself.
  template <class L>
    requires(!std::same_as<std::remove_cvref_t<L>, any_listener> && !std::is_lvalue_reference_v<L> && Listener<L>)
  explicit any_listener(L &&listener)
      : listener_(std::make_unique<L>(std::move(listener)).release()), vtable_(&detail::listener_vtable_for<L>) {}

  any_listener(any_listener const &) = delete;
  any_listener &operator=(any_listener const &) = delete;

  any_listener(any_listener &&other) noexcept
      : listener_(std::exchange(other.listener_, nullptr)), vtable_(std::exchange(other.vtable_, nullptr)) {}

  any_listener &operator=(any_listener &&other) noexcept {
    if (this != &other) {
      reset();
      listener_ = std::exchange(other.listener_, nullptr);
      vtable_ = std::exchange(other.vtable_, nullptr);
    }

    return *this;
  }

  ~any_listener() { reset(); }

  /** @brief The next connection a peer opens. */
  [[nodiscard]] async::task<any_stream> accept() {
    assert(vtable_ != nullptr && "any_listener used after being moved from");
    return vtable_->accept(listener_);
  }

  /** @brief The wrapped listener if its type is exactly `L`, otherwise `nullptr`. */
  template <class L>
  [[nodiscard]] L *target() noexcept {
    return vtable_ != nullptr && vtable_->type == async::detail::type_id_of<L>() ? static_cast<L *>(listener_)
                                                                                 : nullptr;
  }

private:
  void reset() noexcept {
    if (vtable_ != nullptr) {
      vtable_->destroy(listener_);
      vtable_ = nullptr;
      listener_ = nullptr;
    }
  }

  void *listener_;
  detail::listener_vtable const *vtable_;
};

static_assert(Listener<any_listener>);

} // namespace quoll::io

#endif // QUOLL_IO_ANY_LISTENER_HPP
