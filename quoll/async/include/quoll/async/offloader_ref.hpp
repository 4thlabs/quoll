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

#ifndef QUOLL_ASYNC_OFFLOADER_REF_HPP
#define QUOLL_ASYNC_OFFLOADER_REF_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/offloader.hpp>
#include <quoll/async/task.hpp>

#include <concepts>

namespace quoll::async {

/**
 * @brief Non-owning, type-erased reference to an Offloader; models Offloader itself. The service an executor of an
 * event loop offers: `executor.query<async::offloader_ref>()`.
 *
 * Trivially copyable and never allocates. The referenced offloader, usually the executor that answered the query, must
 * outlive the reference.
 *
 * Thread safety: same as the referenced offloader.
 */
class offloader_ref {
public:
  // The exclusion comes first: it stops Offloader<offloader_ref> from checking this constructor against itself.
  template <class T>
    requires(!std::same_as<T, offloader_ref> && Offloader<T>)
  explicit offloader_ref(T const &offloader) noexcept : offloader_(&offloader), vtable_(&vtable_for<T>) {}

  template <class T>
    requires(!std::same_as<T, offloader_ref> && Offloader<T>)
  explicit offloader_ref(T const &&offloader) = delete;

  /** @brief Calls `work(argument)` on another thread (see Offloader). */
  [[nodiscard]] task<> offload(void (*work)(void *) noexcept, void *argument) const {
    return vtable_->offload(offloader_, work, argument);
  }

  /** @brief The referenced offloader if its type is exactly `T`, otherwise `nullptr`. */
  template <class T>
  [[nodiscard]] T const *target() const noexcept {
    return vtable_->type == detail::type_id_of<T>() ? static_cast<T const *>(offloader_) : nullptr;
  }

private:
  struct vtable {
    task<> (*offload)(void const *offloader, void (*work)(void *) noexcept, void *argument);
    detail::type_id type;
  };

  template <Offloader T>
  static constexpr vtable vtable_for{
      [](void const *offloader, void (*work)(void *) noexcept, void *argument) {
        return static_cast<T const *>(offloader)->offload(work, argument);
      },
      detail::type_id_of<T>(),
  };

  void const *offloader_;
  vtable const *vtable_;
};

static_assert(Offloader<offloader_ref>);

} // namespace quoll::async

#endif // QUOLL_ASYNC_OFFLOADER_REF_HPP
