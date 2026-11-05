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

#ifndef QUOLL_HTTP_REDIRECTS_HPP
#define QUOLL_HTTP_REDIRECTS_HPP

#include <map>
#include <string>

#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>

namespace quoll::http {

class redirect_target {
public:
  redirect_target(std::string location) : target(std::move(location)) {}

  std::string const &location() { return target; }

private:
  std::string target;
};

/** @brief Whether a client may follow this response: any status from 300 to 399. */
constexpr bool isRedirect(status code) {
  return code == status::moved_permanently || code == status::found || code == status::see_other ||
         code == status::temporary_redirect || code == status::permanent_redirect;
}

/** @brief The method of the next request after a redirect with this status. */
[[nodiscard]] inline verb method_after_redirect(status code, verb method) noexcept {
  // Check the status.
  bool const was_post = method == verb::post;
  if (code == status::see_other) {
    return method == verb::head ? verb::head : verb::get;
  }
  bool const historic = code == status::moved_permanently || code == status::found;
  if (historic && was_post) {
    return verb::get;
  }
  return method;
}

} // namespace quoll::http

#endif // QUOLL_HTTP_REDIRECTS_HPP
