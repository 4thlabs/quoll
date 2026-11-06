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

#ifndef QUOLL_HTTP_PLATFORM_REF_HPP
#define QUOLL_HTTP_PLATFORM_REF_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/platform.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/streamed_response.hpp>

#include <concepts>
#include <string>
#include <utility>

namespace quoll::http {

/**
 * @brief Non-owning, type-erased reference to a Platform; models Platform itself. The service an executor offers where
 * HTTP goes through the platform: `executor.query<http::platform_ref>()`.
 *
 * Trivially copyable and never allocates. The referenced platform, usually for the whole program, outlives the
 * reference.
 *
 * Thread safety: same as the referenced platform.
 */
class platform_ref {
public:
  // The exclusion comes first: it stops Platform<platform_ref> from checking this constructor against itself.
  template <class P>
    requires(!std::same_as<P, platform_ref> && Platform<P>)
  explicit platform_ref(P const &platform) noexcept : platform_(&platform), vtable_(&vtable_for<P>) {}

  template <class P>
    requires(!std::same_as<P, platform_ref> && Platform<P>)
  explicit platform_ref(P const &&platform) = delete;

  [[nodiscard]] async::task<streamed_response> stream(std::string url, request message) const {
    return vtable_->stream(platform_, std::move(url), std::move(message));
  }

private:
  struct vtable {
    async::task<streamed_response> (*stream)(void const *platform, std::string url, request message);
  };

  template <Platform P>
  static constexpr vtable vtable_for{
      [](void const *platform, std::string url, request message) {
        return static_cast<P const *>(platform)->stream(std::move(url), std::move(message));
      },
  };

  void const *platform_;
  vtable const *vtable_;
};

static_assert(Platform<platform_ref>);

} // namespace quoll::http

#endif // QUOLL_HTTP_PLATFORM_REF_HPP
